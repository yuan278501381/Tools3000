/* ─────────────────────────────────────────────────────────────────────────────
 * CaptureRegionManagerCard — 历史与固定截图选区管理卡片
 *
 * 能力 (MECE 架构):
 *   1. 全局一步复用上次区域快捷键 (Repeat Last Capture，默认 Shift+F1) + 截图内切换单键 (R / Shift+R 等)；
 *   2. 可配置历史选区记忆上限 (0..50，默认 10，跨重启持久化；已置顶固定选区不受淘汰限制)；
 *   3. 完整选区 CRUD 工作台：实时列表、置顶/取消置顶、自定义命名、精准 X/Y/W/H 像素编辑、
 *      一键预设新增 (1280x720 / 1920x1080 / 800x600 / 1080x1080)、按任意选区立即截图、清空未置顶。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { Camera, Crop, Pencil, Pin, PinOff, Plus, Trash2, Check, X } from 'lucide-react';
import { toast } from 'sonner';
import { Button, Card, CodeBadge, Select, SettingGroup, SettingRow } from './UIKit';
import { HotkeyRecorder } from './HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from './HotkeyStatusBadge';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import './CaptureRegionManagerCard.css';

export interface SavedCaptureRegion {
  id: string;
  l: number;
  t: number;
  r: number;
  b: number;
  name: string;
  pinned: boolean;
  updatedAt: number;
}

interface RegionHistoryStateResponse {
  success?: boolean;
  error?: string;
  items?: SavedCaptureRegion[];
  maxEntries?: number;
  pinnedCount?: number;
  primaryWidth?: number;
  primaryHeight?: number;
}

interface CaptureRegionManagerCardProps {
  repeatCaptureHotkey: string;
  repeatCaptureHotkeyEntry?: HotkeyEntry;
  onRebindRepeatCaptureHotkey: (value: string) => void;
  regionHistoryLimit: number;
  regionHistoryKey: string;
  onUpdateRegionHistoryLimit: (limit: number) => void;
  onUpdateRegionHistoryKey: (keyName: string) => void;
}

const REGION_LIMIT_OPTIONS = [0, 5, 10, 20, 30, 50] as const;

const QUICK_SIZE_PRESETS = [
  { label: '1280x720', w: 1280, h: 720 },
  { label: '1920x1080', w: 1920, h: 1080 },
  { label: '1080x1080', w: 1080, h: 1080 },
  { label: '800x600', w: 800, h: 600 },
] as const;

interface DraftForm {
  id: string;
  name: string;
  x: string;
  y: string;
  w: string;
  h: string;
  pinned: boolean;
}

function createCenteredDraft(primaryW: number, primaryH: number, w = 1280, h = 720): DraftForm {
  const safeW = Math.max(4, Math.min(w, Math.max(400, primaryW)));
  const safeH = Math.max(4, Math.min(h, Math.max(300, primaryH)));
  const x = Math.max(0, Math.round((primaryW - safeW) / 2));
  const y = Math.max(0, Math.round((primaryH - safeH) / 2));
  return {
    id: '',
    name: '',
    x: String(x),
    y: String(y),
    w: String(safeW),
    h: String(safeH),
    pinned: true,
  };
}

export const CaptureRegionManagerCard: FC<CaptureRegionManagerCardProps> = ({
  repeatCaptureHotkey,
  repeatCaptureHotkeyEntry,
  onRebindRepeatCaptureHotkey,
  regionHistoryLimit,
  regionHistoryKey,
  onUpdateRegionHistoryLimit,
  onUpdateRegionHistoryKey,
}) => {
  const { t } = useTranslation();
  const [items, setItems] = useState<SavedCaptureRegion[]>([]);
  const [primarySize, setPrimarySize] = useState({ w: 1920, h: 1080 });
  const [editorOpen, setEditorOpen] = useState(false);
  const [draft, setDraft] = useState<DraftForm>(() => createCenteredDraft(1920, 1080));
  const [submitting, setSubmitting] = useState(false);

  const applyServerPayload = useCallback((raw: unknown) => {
    if (!raw || typeof raw !== 'object') return;
    const data = raw as RegionHistoryStateResponse;
    if (Array.isArray(data.items)) {
      setItems(data.items);
    }
    if (
      typeof data.primaryWidth === 'number' &&
      data.primaryWidth >= 320 &&
      typeof data.primaryHeight === 'number' &&
      data.primaryHeight >= 240
    ) {
      setPrimarySize({ w: data.primaryWidth, h: data.primaryHeight });
    }
  }, []);

  const fetchRegions = useCallback(() => {
    bridgeRequest<RegionHistoryStateResponse>('capture.getRegionHistory', {}, { silent: true })
      .then((res) => applyServerPayload(res))
      .catch(() => { /* keep interactive */ });
  }, [applyServerPayload]);

  useEffect(() => {
    fetchRegions();
  }, [fetchRegions]);

  useBridgeEvent('capture:regionHistoryChanged', (payload) => {
    applyServerPayload(payload);
  });

  const pinnedCount = items.filter((item) => item.pinned).length;
  const unpinnedCount = items.length - pinnedCount;

  const openCreateEditor = () => {
    setDraft(createCenteredDraft(primarySize.w, primarySize.h, 1280, 720));
    setEditorOpen(true);
  };

  const openEditEditor = (entry: SavedCaptureRegion) => {
    setDraft({
      id: entry.id,
      name: entry.name || '',
      x: String(entry.l),
      y: String(entry.t),
      w: String(Math.max(4, entry.r - entry.l)),
      h: String(Math.max(4, entry.b - entry.t)),
      pinned: entry.pinned,
    });
    setEditorOpen(true);
  };

  const applyPresetSize = (w: number, h: number) => {
    const centered = createCenteredDraft(primarySize.w, primarySize.h, w, h);
    setDraft((prev) => ({
      ...prev,
      x: centered.x,
      y: centered.y,
      w: centered.w,
      h: centered.h,
    }));
  };

  const handleSaveDraft = async () => {
    if (submitting) return;
    const x = Number.parseInt(draft.x, 10);
    const y = Number.parseInt(draft.y, 10);
    const w = Number.parseInt(draft.w, 10);
    const h = Number.parseInt(draft.h, 10);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(w) || !Number.isFinite(h) || w < 4 || h < 4) {
      toast.error(t('capture.regionInvalidSize'));
      return;
    }
    setSubmitting(true);
    try {
      const res = await bridgeRequest<RegionHistoryStateResponse>('capture.upsertRegionHistory', {
        id: draft.id,
        name: draft.name.trim(),
        l: x,
        t: y,
        r: x + w,
        b: y + h,
        pinned: draft.pinned,
      }, { silent: true });
      if (!res?.success) {
        throw new Error(res?.error || t('capture.saveFailed'));
      }
      applyServerPayload(res);
      setEditorOpen(false);
    } catch (error) {
      toast.error(t('capture.saveFailed'), { description: String(error) });
    } finally {
      setSubmitting(false);
    }
  };

  const handleTogglePin = async (entry: SavedCaptureRegion) => {
    try {
      const res = await bridgeRequest<RegionHistoryStateResponse>('capture.setRegionPinned', {
        id: entry.id,
        pinned: !entry.pinned,
      }, { silent: true });
      if (!res?.success) throw new Error(res?.error || t('capture.saveFailed'));
      applyServerPayload(res);
    } catch (error) {
      toast.error(t('capture.saveFailed'), { description: String(error) });
    }
  };

  const handleDelete = async (entry: SavedCaptureRegion) => {
    try {
      const res = await bridgeRequest<RegionHistoryStateResponse>('capture.deleteRegion', {
        id: entry.id,
      }, { silent: true });
      if (!res?.success) throw new Error(res?.error || t('capture.saveFailed'));
      applyServerPayload(res);
    } catch (error) {
      toast.error(t('capture.saveFailed'), { description: String(error) });
    }
  };

  const handleClearUnpinned = async () => {
    try {
      const res = await bridgeRequest<RegionHistoryStateResponse>('capture.clearRegionHistory', {
        includePinned: false,
      }, { silent: true });
      if (!res?.success) throw new Error(res?.error || t('capture.saveFailed'));
      applyServerPayload(res);
      toast.success(t('capture.regionClearedToast'));
    } catch (error) {
      toast.error(t('capture.saveFailed'), { description: String(error) });
    }
  };

  const handleCaptureWithRegion = async (entry: SavedCaptureRegion) => {
    try {
      const res = await bridgeRequest<{ success?: boolean; error?: string }>('capture.triggerWithRegion', {
        id: entry.id,
      }, { silent: true });
      if (!res?.success) throw new Error(res?.error || t('capture.startFailed'));
    } catch (error) {
      toast.error(t('capture.startFailed'), { description: String(error) });
    }
  };

  const limitSelectOptions = (() => {
    const vals: number[] = [...REGION_LIMIT_OPTIONS];
    if (!vals.includes(regionHistoryLimit)) {
      vals.push(regionHistoryLimit);
      vals.sort((a, b) => a - b);
    }
    return vals.map((n) => ({
      value: String(n),
      label: n === 0
        ? t('capture.regionLimitOff')
        : t('capture.regionLimitCount', { count: n }),
    }));
  })();

  const cycleKeyOptions = [
    { value: 'R', label: t('capture.regionKeyR') },
    { value: '`', label: t('capture.regionKeyBackquote') },
    { value: 'F2', label: t('capture.regionKeyF2') },
    { value: 'F4', label: t('capture.regionKeyF4') },
    { value: '', label: t('capture.regionKeyOff') },
  ];

  return (
    <SettingGroup title={t('capture.regionManagerTitle')} icon={<Crop size={20} strokeWidth={2.5} />}>
      <Card>
        <div className="region-mgr">
          <SettingRow
            label={
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                <span>{t('capture.repeatLastCaptureShortcut')}</span>
                <HotkeyStatusBadge entry={repeatCaptureHotkeyEntry} />
              </div>
            }
            description={t('capture.repeatLastCaptureShortcutDesc')}
          >
            <HotkeyRecorder
              id="capture-repeat-last-shortcut"
              value={repeatCaptureHotkey}
              onChange={onRebindRepeatCaptureHotkey}
            />
          </SettingRow>

          <SettingRow
            label={t('capture.regionCycleKeyLabel')}
            description={t('capture.regionCycleKeyDesc')}
          >
            <Select
              id="capture-region-cycle-key"
              value={regionHistoryKey}
              onChange={onUpdateRegionHistoryKey}
              options={cycleKeyOptions}
            />
          </SettingRow>

          <SettingRow
            label={t('capture.regionHistoryLimitLabel')}
            description={t('capture.regionHistoryLimitDesc')}
          >
            <Select
              id="capture-region-history-limit"
              value={String(regionHistoryLimit)}
              onChange={(v) => onUpdateRegionHistoryLimit(parseInt(v, 10))}
              options={limitSelectOptions}
            />
          </SettingRow>

          <div className="region-mgr__divider" />

          <div className="region-mgr__toolbar">
            <div className="region-mgr__summary">
              <span>{t('capture.regionSummary', { total: items.length, pinned: pinnedCount })}</span>
            </div>
            <div className="region-mgr__toolbar-actions">
              <Button id="capture-region-add-btn" variant="ghost" onClick={openCreateEditor}>
                <Plus size={14} aria-hidden="true" />
                <span>{t('capture.regionAddFixed')}</span>
              </Button>
              <Button
                id="capture-region-clear-btn"
                variant="ghost"
                disabled={unpinnedCount === 0}
                onClick={() => void handleClearUnpinned()}
              >
                <Trash2 size={14} aria-hidden="true" />
                <span>{t('capture.regionClearUnpinned')}</span>
              </Button>
            </div>
          </div>

          {editorOpen && (
            <div className="region-mgr__editor">
              <div className="region-mgr__presets">
                <span className="region-mgr__preset-label">{t('capture.regionQuickPresets')}</span>
                {QUICK_SIZE_PRESETS.map((preset) => (
                  <button
                    key={preset.label}
                    type="button"
                    className="region-mgr__preset-btn"
                    onClick={() => applyPresetSize(preset.w, preset.h)}
                  >
                    {preset.label}
                  </button>
                ))}
              </div>

              <div className="region-mgr__form-grid">
                <label className="region-mgr__field">
                  <span className="region-mgr__field-label">{t('capture.regionFieldName')}</span>
                  <input
                    id="capture-region-name-input"
                    type="text"
                    className="region-mgr__input"
                    maxLength={48}
                    value={draft.name}
                    placeholder={t('capture.regionNamePlaceholder')}
                    onChange={(e) => setDraft((prev) => ({ ...prev, name: e.target.value }))}
                  />
                </label>
                <label className="region-mgr__field">
                  <span className="region-mgr__field-label">X</span>
                  <input
                    type="number"
                    className="region-mgr__input region-mgr__input--num"
                    value={draft.x}
                    onChange={(e) => setDraft((prev) => ({ ...prev, x: e.target.value }))}
                  />
                </label>
                <label className="region-mgr__field">
                  <span className="region-mgr__field-label">Y</span>
                  <input
                    type="number"
                    className="region-mgr__input region-mgr__input--num"
                    value={draft.y}
                    onChange={(e) => setDraft((prev) => ({ ...prev, y: e.target.value }))}
                  />
                </label>
                <label className="region-mgr__field">
                  <span className="region-mgr__field-label">{t('capture.regionFieldWidth')}</span>
                  <input
                    type="number"
                    min={4}
                    className="region-mgr__input region-mgr__input--num"
                    value={draft.w}
                    onChange={(e) => setDraft((prev) => ({ ...prev, w: e.target.value }))}
                  />
                </label>
                <label className="region-mgr__field">
                  <span className="region-mgr__field-label">{t('capture.regionFieldHeight')}</span>
                  <input
                    type="number"
                    min={4}
                    className="region-mgr__input region-mgr__input--num"
                    value={draft.h}
                    onChange={(e) => setDraft((prev) => ({ ...prev, h: e.target.value }))}
                  />
                </label>
              </div>

              <div className="region-mgr__editor-actions">
                <Button variant="ghost" onClick={() => setEditorOpen(false)}>
                  <X size={14} aria-hidden="true" />
                  <span>{t('common.cancel')}</span>
                </Button>
                <Button id="capture-region-save-btn" variant="primary" disabled={submitting} onClick={() => void handleSaveDraft()}>
                  <Check size={14} aria-hidden="true" />
                  <span>{t('common.save')}</span>
                </Button>
              </div>
            </div>
          )}

          {items.length === 0 ? (
            <div className="region-mgr__empty">{t('capture.regionEmptyHint')}</div>
          ) : (
            <div className="region-mgr__list">
              {items.map((entry, idx) => {
                const width = Math.max(0, entry.r - entry.l);
                const height = Math.max(0, entry.b - entry.t);
                const displayName = entry.name || t('capture.regionUnnamedLabel', { index: idx + 1 });
                return (
                  <div
                    key={entry.id}
                    className={`region-mgr__row${entry.pinned ? ' region-mgr__row--pinned' : ''}`}
                  >
                    <div className="region-mgr__meta">
                      <span className="region-mgr__index">{`#${idx + 1}`}</span>
                      <span className="region-mgr__name" title={displayName}>{displayName}</span>
                      {entry.pinned && (
                        <span className="region-mgr__pinned-tag">
                          <Pin size={11} aria-hidden="true" />
                          <span>{t('capture.regionPinnedBadge')}</span>
                        </span>
                      )}
                      <CodeBadge>{`${width} x ${height}`}</CodeBadge>
                      <CodeBadge>{`(${entry.l}, ${entry.t})`}</CodeBadge>
                    </div>

                    <div className="region-mgr__actions">
                      <button
                        type="button"
                        className="region-mgr__icon-btn"
                        title={t('capture.regionCaptureNow')}
                        aria-label={t('capture.regionCaptureNow')}
                        onClick={() => void handleCaptureWithRegion(entry)}
                      >
                        <Camera size={13} aria-hidden="true" />
                        <span>{t('capture.regionCaptureNowShort')}</span>
                      </button>
                      <button
                        type="button"
                        className={`region-mgr__icon-btn${entry.pinned ? ' region-mgr__icon-btn--active' : ''}`}
                        title={entry.pinned ? t('capture.regionUnpin') : t('capture.regionPin')}
                        aria-label={entry.pinned ? t('capture.regionUnpin') : t('capture.regionPin')}
                        onClick={() => void handleTogglePin(entry)}
                      >
                        {entry.pinned ? <PinOff size={13} aria-hidden="true" /> : <Pin size={13} aria-hidden="true" />}
                      </button>
                      <button
                        type="button"
                        className="region-mgr__icon-btn"
                        title={t('capture.regionEdit')}
                        aria-label={t('capture.regionEdit')}
                        onClick={() => openEditEditor(entry)}
                      >
                        <Pencil size={13} aria-hidden="true" />
                      </button>
                      <button
                        type="button"
                        className="region-mgr__icon-btn"
                        title={t('capture.regionDelete')}
                        aria-label={t('capture.regionDelete')}
                        onClick={() => void handleDelete(entry)}
                      >
                        <Trash2 size={13} aria-hidden="true" />
                      </button>
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </div>
      </Card>
    </SettingGroup>
  );
};

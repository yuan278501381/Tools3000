import { useRef, useState } from 'react';
import {
  Layers,
  ChevronDown,
  Plus,
  FolderPlus,
  LayoutGrid,
  Unlink,
  Trash2,
  Eye,
  EyeOff,
  Palette,
  RotateCcw,
  Check,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import {
  DEFAULT_PIN_GROUP_PRESET_COLORS,
  type PinGroupControlPayload,
  type TrayPinGroupEntry,
  type TrayPinGroupState,
} from './trayRegistry';

interface TrayPinGroupPanelProps {
  state: TrayPinGroupState;
  expanded: boolean;
  disabled?: boolean;
  onToggleExpand: () => void;
  onControl: (payload: PinGroupControlPayload) => void;
}

function buildFallbackGroupEntry(id: number): TrayPinGroupEntry {
  const preset = DEFAULT_PIN_GROUP_PRESET_COLORS[(id - 1) % DEFAULT_PIN_GROUP_PRESET_COLORS.length];
  return {
    id,
    count: 0,
    shortLabel: `G${id}`,
    fullLabel: `Group ${id} (${preset.name})`,
    customName: '',
    customColorHex: '',
    defaultColorHex: preset.hex,
    colorHex: preset.hex,
  };
}

export function TrayPinGroupPanel({
  state,
  expanded,
  disabled = false,
  onToggleExpand,
  onControl,
}: TrayPinGroupPanelProps) {
  const { t } = useTranslation();
  const [customizeOpen, setCustomizeOpen] = useState(false);
  const [editingGroupId, setEditingGroupId] = useState<number>(1);
  const [draftOverrides, setDraftOverrides] = useState<Record<number, string>>({});
  const skipCommitOnBlurRef = useRef(false);

  const groupCount = state.groups.length;
  const hasAnyContent = state.totalCount > 0 || groupCount > 0;
  const canCreateMore = state.nextAvailableGroupId > 0;
  const isGroupSelected = state.activeFilter >= 1;
  const palettePresets = state.palettePresets?.length
    ? state.palettePresets
    : DEFAULT_PIN_GROUP_PRESET_COLORS;

  const editableCatalog: TrayPinGroupEntry[] = (() => {
    if (state.catalog && state.catalog.length > 0) {
      return state.catalog;
    }
    const byId = new Map<number, TrayPinGroupEntry>();
    for (let id = 1; id <= 6; id += 1) {
      byId.set(id, buildFallbackGroupEntry(id));
    }
    for (const grp of state.groups) {
      byId.set(grp.id, grp);
    }
    return Array.from(byId.values()).sort((a, b) => a.id - b.id);
  })();

  const activeTargetId = isGroupSelected ? state.activeFilter : editingGroupId;
  const targetEntry =
    state.groups.find((g) => g.id === activeTargetId) ??
    editableCatalog.find((g) => g.id === activeTargetId) ??
    buildFallbackGroupEntry(activeTargetId);

  const showMetaEditor = isGroupSelected || customizeOpen;
  const serverCustomName = targetEntry.customName ?? '';
  const nameDraft = draftOverrides[targetEntry.id] ?? serverCustomName;

  const clearDraftOverride = (id: number) => {
    setDraftOverrides((prev) => {
      if (!(id in prev)) return prev;
      const next = { ...prev };
      delete next[id];
      return next;
    });
  };

  const commitNameDraft = () => {
    const trimmed = nameDraft.trim().slice(0, 24);
    const prev = serverCustomName.trim();
    clearDraftOverride(targetEntry.id);
    if (trimmed === prev) return;
    onControl({
      updateGroupMeta: {
        id: targetEntry.id,
        name: trimmed,
      },
    });
  };

  const handleSelectColor = (hex: string) => {
    const normalized = hex.toUpperCase();
    if (normalized === targetEntry.colorHex.toUpperCase()) return;
    onControl({
      updateGroupMeta: {
        id: targetEntry.id,
        colorHex: normalized,
      },
    });
  };

  const hasCustomMeta =
    Boolean(targetEntry.hasCustomization) ||
    Boolean(targetEntry.customName && targetEntry.customName.trim().length > 0) ||
    Boolean(targetEntry.customColorHex && targetEntry.customColorHex.trim().length > 0);

  const summaryBadge = groupCount > 0
    ? t('tray.pinGroupSummary', {
        defaultValue: '{{count}} Pins · {{groups}} Groups',
        count: state.totalCount,
        groups: groupCount,
      })
    : state.totalCount > 0
      ? t('tray.pinCountSummary', {
          defaultValue: '{{count}} Pins',
          count: state.totalCount,
        })
      : '';

  const previewDots = (state.groups.length > 0 ? state.groups : editableCatalog).slice(0, 4);

  return (
    <div className={`tray-pin-section ${expanded ? 'tray-pin-section--expanded' : ''}`}>
      <button
        type="button"
        role="menuitem"
        aria-expanded={expanded}
        className={`tray-pin-card__trigger ${expanded ? 'tray-pin-card__trigger--expanded' : ''}`}
        disabled={disabled}
        onClick={onToggleExpand}
      >
        <span className="tray-pin-card__icon-wrap" aria-hidden="true">
          <Layers size={13} className="tray-pin-card__icon" />
        </span>
        <span className="tray-pin-card__title">{t('tray.pinGroups', 'Pin Groups')}</span>
        {summaryBadge ? (
          <span className="tray-pin-count-pill">{summaryBadge}</span>
        ) : (
          <span className="tray-pin-color-dots" aria-hidden="true">
            {previewDots.map((grp) => (
              <span
                key={grp.id}
                className="tray-pin-color-dots__dot"
                style={{ backgroundColor: grp.colorHex }}
              />
            ))}
          </span>
        )}
        <ChevronDown
          size={14}
          className={`tray-pin-chevron ${expanded ? 'tray-pin-chevron--open' : ''}`}
          aria-hidden="true"
        />
      </button>

      {expanded && (
        <div className="tray-pin-drawer" role="region" aria-label={t('tray.pinGroups', 'Pin Groups')}>
          <div className="tray-pin-drawer__top">
            <span className="tray-pin-drawer__hint">
              {!hasAnyContent
                ? t('tray.pinEmptyHint', 'No active pins yet')
                : !canCreateMore
                  ? t('tray.pinMaxReached', 'Max 32 groups reached')
                  : t('tray.pinCountSummary', {
                      defaultValue: '{{count}} Pins',
                      count: state.totalCount,
                    })}
            </span>
            <div className="tray-pin-drawer__top-btns">
              <button
                type="button"
                className={`tray-pin-mini-btn ${customizeOpen && !isGroupSelected ? 'tray-pin-mini-btn--active' : ''}`}
                disabled={disabled}
                aria-pressed={showMetaEditor}
                title={t('tray.pinCustomizeGroupTip', 'Customize group name and color')}
                onClick={() => setCustomizeOpen((prev) => !prev)}
              >
                <Palette size={12} aria-hidden="true" />
                <span>{t('tray.pinCustomizeBtn', 'Style')}</span>
              </button>
              <button
                type="button"
                className="tray-pin-mini-btn tray-pin-mini-btn--primary"
                disabled={disabled || !canCreateMore}
                onClick={() => onControl({ createNewGroup: true })}
              >
                <Plus size={12} aria-hidden="true" />
                <span>{t('tray.pinNewGroup', 'New Group')}</span>
              </button>
              {state.ungroupedCount >= 1 && (
                <button
                  type="button"
                  className="tray-pin-mini-btn"
                  disabled={disabled}
                  onClick={() => onControl({ autoGroupUngrouped: true })}
                >
                  <FolderPlus size={12} aria-hidden="true" />
                  <span>{t('tray.pinAutoGroup', 'Auto Group')}</span>
                </button>
              )}
            </div>
          </div>

          {hasAnyContent && (
            <div className="tray-pin-chips">
              <button
                type="button"
                className={`tray-pin-chip ${state.activeFilter === -1 ? 'tray-pin-chip--active' : ''}`}
                disabled={disabled}
                onClick={() => onControl({ filterGroup: -1 })}
              >
                <span>
                  {t('tray.pinViewAll', {
                    defaultValue: 'All {{count}}',
                    count: state.totalCount,
                  })}
                </span>
              </button>

              {state.groups.map((grp) => {
                const active = state.activeFilter === grp.id;
                return (
                  <button
                    key={grp.id}
                    type="button"
                    className={`tray-pin-chip ${active ? 'tray-pin-chip--active' : ''}`}
                    disabled={disabled}
                    title={grp.fullLabel}
                    onClick={() => {
                      setEditingGroupId(grp.id);
                      onControl({ filterGroup: active ? -1 : grp.id });
                    }}
                  >
                    <span
                      className="tray-pin-chip__dot"
                      style={{ backgroundColor: grp.colorHex }}
                      aria-hidden="true"
                    />
                    <span>{grp.shortLabel}</span>
                    <span className="tray-pin-chip__num">{grp.count}</span>
                  </button>
                );
              })}

              {(state.ungroupedCount > 0 || state.activeFilter === 0) && (
                <button
                  type="button"
                  className={`tray-pin-chip ${state.activeFilter === 0 ? 'tray-pin-chip--active' : ''}`}
                  disabled={disabled}
                  onClick={() => onControl({ filterGroup: state.activeFilter === 0 ? -1 : 0 })}
                >
                  <span>
                    {t('tray.pinViewUngrouped', {
                      defaultValue: 'Ungrouped {{count}}',
                      count: state.ungroupedCount,
                    })}
                  </span>
                </button>
              )}
            </div>
          )}

          {showMetaEditor && (
            <div className="tray-pin-meta-editor">
              {!isGroupSelected && (
                <div className="tray-pin-meta-targets">
                  {editableCatalog.map((grp) => {
                    const selected = grp.id === activeTargetId;
                    return (
                      <button
                        key={grp.id}
                        type="button"
                        className={`tray-pin-target-pill ${selected ? 'tray-pin-target-pill--selected' : ''}`}
                        disabled={disabled}
                        onClick={() => setEditingGroupId(grp.id)}
                      >
                        <span
                          className="tray-pin-chip__dot"
                          style={{ backgroundColor: grp.colorHex }}
                          aria-hidden="true"
                        />
                        <span>{grp.shortLabel}</span>
                      </button>
                    );
                  })}
                </div>
              )}

              <div className="tray-pin-meta-name-row">
                <span
                  className="tray-pin-meta-badge"
                  style={{ borderColor: targetEntry.colorHex, color: targetEntry.colorHex }}
                >
                  {`#${targetEntry.id}`}
                </span>
                <input
                  type="text"
                  className="tray-pin-meta-input"
                  value={nameDraft}
                  maxLength={24}
                  disabled={disabled}
                  aria-label={t('tray.pinGroupNameInputAria', {
                    defaultValue: 'Group {{id}} custom name',
                    id: targetEntry.id,
                  })}
                  placeholder={t('tray.pinGroupNamePlaceholder', {
                    defaultValue: 'Group {{id}} name...',
                    id: targetEntry.id,
                  })}
                  onChange={(e) =>
                    setDraftOverrides((prev) => ({
                      ...prev,
                      [targetEntry.id]: e.target.value,
                    }))
                  }
                  onBlur={() => {
                    if (skipCommitOnBlurRef.current) {
                      skipCommitOnBlurRef.current = false;
                      return;
                    }
                    commitNameDraft();
                  }}
                  onKeyDown={(e) => {
                    if (e.nativeEvent.isComposing || e.keyCode === 229) {
                      return;
                    }
                    if (e.key === 'Enter') {
                      e.preventDefault();
                      commitNameDraft();
                    } else if (e.key === 'Escape') {
                      e.preventDefault();
                      e.stopPropagation();
                      skipCommitOnBlurRef.current = true;
                      clearDraftOverride(targetEntry.id);
                      try {
                        e.currentTarget.blur();
                      } finally {
                        skipCommitOnBlurRef.current = false;
                      }
                    }
                  }}
                />
                <button
                  type="button"
                  className="tray-pin-meta-icon-btn"
                  disabled={disabled || nameDraft.trim() === serverCustomName.trim()}
                  title={t('tray.pinSaveGroupName', 'Save group name')}
                  aria-label={t('tray.pinSaveGroupName', 'Save group name')}
                  onClick={commitNameDraft}
                >
                  <Check size={12} aria-hidden="true" />
                </button>
                {hasCustomMeta && (
                  <button
                    type="button"
                    className="tray-pin-meta-icon-btn"
                    disabled={disabled}
                    title={t('tray.pinResetGroupMeta', 'Reset group name & color')}
                    aria-label={t('tray.pinResetGroupMeta', 'Reset group name & color')}
                    onClick={() => {
                      clearDraftOverride(targetEntry.id);
                      onControl({ resetGroupMeta: targetEntry.id });
                    }}
                  >
                    <RotateCcw size={12} aria-hidden="true" />
                  </button>
                )}
              </div>

              <div className="tray-pin-swatches" role="radiogroup" aria-label={t('tray.pinGroupColorLabel', 'Group Color')}>
                {palettePresets.map((preset) => {
                  const selected = preset.hex.toUpperCase() === targetEntry.colorHex.toUpperCase();
                  return (
                    <button
                      key={preset.slot}
                      type="button"
                      role="radio"
                      aria-checked={selected}
                      aria-label={`${preset.name} ${preset.hex}`}
                      title={`${preset.name} (${preset.hex})`}
                      disabled={disabled}
                      className={`tray-pin-swatch ${selected ? 'tray-pin-swatch--selected' : ''}`}
                      style={{ backgroundColor: preset.hex }}
                      onClick={() => handleSelectColor(preset.hex)}
                    />
                  );
                })}
              </div>
            </div>
          )}

          {isGroupSelected && (
            <div className="tray-pin-actions">
              <button
                type="button"
                className="tray-pin-action-btn"
                disabled={disabled}
                onClick={() => onControl({ arrangeGroup: state.activeFilter })}
              >
                <LayoutGrid size={12} aria-hidden="true" />
                <span>{t('tray.pinArrangeGroup', 'Arrange')}</span>
              </button>
              <button
                type="button"
                className="tray-pin-action-btn"
                disabled={disabled}
                onClick={() => onControl({ dissolveGroup: state.activeFilter })}
              >
                <Unlink size={12} aria-hidden="true" />
                <span>{t('tray.pinDissolveGroup', 'Ungroup')}</span>
              </button>
              <button
                type="button"
                className="tray-pin-action-btn tray-pin-action-btn--danger"
                disabled={disabled}
                onClick={() => onControl({ closeGroup: state.activeFilter })}
              >
                <Trash2 size={12} aria-hidden="true" />
                <span>{t('tray.pinCloseGroup', 'Close Group')}</span>
              </button>
            </div>
          )}

          {state.totalCount > 0 && (
            <div className="tray-pin-actions tray-pin-actions--global">
              <button
                type="button"
                className="tray-pin-action-btn"
                disabled={disabled}
                onClick={() => onControl({ arrangeAll: true })}
              >
                <LayoutGrid size={12} aria-hidden="true" />
                <span>{t('tray.pinArrangeAll', 'Arrange All')}</span>
              </button>
              <button
                type="button"
                className="tray-pin-action-btn"
                disabled={disabled}
                onClick={() => onControl({ toggleHideAll: true })}
              >
                {state.allHidden ? (
                  <Eye size={12} aria-hidden="true" />
                ) : (
                  <EyeOff size={12} aria-hidden="true" />
                )}
                <span>
                  {state.allHidden
                    ? t('tray.pinShowAll', 'Show All')
                    : t('tray.pinHideAll', 'Hide All')}
                </span>
              </button>
              <button
                type="button"
                className="tray-pin-action-btn tray-pin-action-btn--danger"
                disabled={disabled}
                onClick={() => onControl({ closeAll: true })}
              >
                <Trash2 size={12} aria-hidden="true" />
                <span>{t('tray.pinCloseAll', 'Close All')}</span>
              </button>
            </div>
          )}
        </div>
      )}
    </div>
  );
}

import { useState, useEffect, useCallback, useRef, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { Layers, Plus, RotateCcw, Pipette } from 'lucide-react';
import { Card, SettingGroup, Button, CodeBadge } from './UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import {
  DEFAULT_PIN_GROUP_PRESET_COLORS,
  normalizePinGroupState,
  type PinGroupControlPayload,
  type TrayPinGroupEntry,
  type TrayPinGroupPresetColor,
} from '../tray/trayRegistry';
import './PinGroupSettingsCard.css';

function buildDefaultCatalog(presets: TrayPinGroupPresetColor[]): TrayPinGroupEntry[] {
  const list: TrayPinGroupEntry[] = [];
  for (let id = 1; id <= 6; id += 1) {
    const preset = presets[(id - 1) % presets.length];
    list.push({
      id,
      count: 0,
      shortLabel: `G${id}`,
      fullLabel: `Group ${id} (${preset.name})`,
      customName: '',
      customColorHex: '',
      defaultColorHex: preset.hex,
      colorHex: preset.hex,
    });
  }
  return list;
}

export const PinGroupSettingsCard: FC = () => {
  const { t } = useTranslation();
  const [presets, setPresets] = useState<TrayPinGroupPresetColor[]>(DEFAULT_PIN_GROUP_PRESET_COLORS);
  const [catalog, setCatalog] = useState<TrayPinGroupEntry[]>(() =>
    buildDefaultCatalog(DEFAULT_PIN_GROUP_PRESET_COLORS)
  );
  const [maxGroupId, setMaxGroupId] = useState<number>(32);
  const [draftNames, setDraftNames] = useState<Record<number, string>>({});
  const skipBlurIdRef = useRef<number | null>(null);

  const applyServerState = useCallback((raw: unknown) => {
    const normalized = normalizePinGroupState(raw);
    const nextPresets = normalized.palettePresets?.length
      ? normalized.palettePresets
      : DEFAULT_PIN_GROUP_PRESET_COLORS;
    setPresets(nextPresets);
    setMaxGroupId(normalized.maxGroupId || 32);

    let nextCatalog: TrayPinGroupEntry[];
    if (normalized.catalog && normalized.catalog.length > 0) {
      nextCatalog = normalized.catalog;
    } else {
      const merged = new Map<number, TrayPinGroupEntry>();
      for (const item of buildDefaultCatalog(nextPresets)) {
        merged.set(item.id, item);
      }
      for (const item of normalized.groups) {
        merged.set(item.id, item);
      }
      nextCatalog = Array.from(merged.values()).sort((a, b) => a.id - b.id);
    }

    setCatalog(nextCatalog);
    setDraftNames(() => {
      const map: Record<number, string> = {};
      for (const entry of nextCatalog) {
        map[entry.id] = entry.customName ?? '';
      }
      return map;
    });
  }, []);

  const fetchCatalog = useCallback(() => {
    bridgeRequest('capture.pinGroupControl', {}, { silent: true })
      .then((res) => applyServerState(res))
      .catch(() => {
        /* fallback catalog stays interactive */
      });
  }, [applyServerState]);

  useEffect(() => {
    fetchCatalog();
  }, [fetchCatalog]);

  useBridgeEvent('pin:groupsChanged', (payload) => {
    applyServerState(payload);
  });

  const dispatchControl = useCallback(
    (payload: PinGroupControlPayload) => {
      bridgeRequest('capture.pinGroupControl', payload as Record<string, unknown>, { silent: true })
        .then((res) => applyServerState(res))
        .catch(() => {});
    },
    [applyServerState]
  );

  const handleCommitName = (entry: TrayPinGroupEntry) => {
    const draft = (draftNames[entry.id] ?? '').trim().slice(0, 24);
    const current = (entry.customName ?? '').trim();
    if (draft === current) return;
    setCatalog((prev) =>
      prev.map((item) =>
        item.id === entry.id
          ? {
              ...item,
              customName: draft,
              shortLabel: draft || `G${entry.id}`,
              hasCustomization: Boolean(draft || item.customColorHex),
            }
          : item
      )
    );
    dispatchControl({
      updateGroupMeta: {
        id: entry.id,
        name: draft,
      },
    });
  };

  const handleSelectColor = (entry: TrayPinGroupEntry, hex: string) => {
    const upper = hex.toUpperCase();
    if (upper === entry.colorHex.toUpperCase()) return;
    setCatalog((prev) =>
      prev.map((item) =>
        item.id === entry.id
          ? {
              ...item,
              colorHex: upper,
              customColorHex: upper,
              hasCustomization: true,
            }
          : item
      )
    );
    dispatchControl({
      updateGroupMeta: {
        id: entry.id,
        colorHex: upper,
      },
    });
  };

  const handleAddNextSlot = () => {
    const usedIds = new Set(catalog.map((item) => item.id));
    let nextId = 1;
    while (nextId <= maxGroupId && usedIds.has(nextId)) {
      nextId += 1;
    }
    if (nextId > maxGroupId) return;
    const preset = presets[(nextId - 1) % presets.length];
    const defaultName = t('capture.pinGroupNewDefaultName', {
      defaultValue: 'Group {{id}}',
      id: nextId,
    });
    setCatalog((prev) =>
      [
        ...prev,
        {
          id: nextId,
          count: 0,
          explicitCreated: true,
          hasCustomization: true,
          shortLabel: defaultName,
          fullLabel: `${defaultName} (#${nextId})`,
          customName: defaultName,
          customColorHex: preset.hex,
          defaultColorHex: preset.hex,
          colorHex: preset.hex,
        },
      ].sort((a, b) => a.id - b.id)
    );
    setDraftNames((prev) => ({ ...prev, [nextId]: defaultName }));
    dispatchControl({
      updateGroupMeta: {
        id: nextId,
        name: defaultName,
        colorHex: preset.hex,
        ensureCreated: true,
      },
    });
  };

  const anyCustomized = catalog.some(
    (item) =>
      Boolean(item.hasCustomization) ||
      Boolean(item.customName && item.customName.trim().length > 0) ||
      Boolean(item.customColorHex && item.customColorHex.trim().length > 0)
  );
  const canAddMore = catalog.length < maxGroupId;

  return (
    <SettingGroup
      title={t('capture.pinGroupsSectionTitle', 'Pin Groups & Color Palette')}
      icon={<Layers size={20} strokeWidth={2.5} />}
    >
      <Card
        title={t('capture.pinGroupsCardTitle', 'Custom Group Names & Accent Colors')}
        subtitle={t(
          'capture.pinGroupsCardSubtitle',
          'Customize names and colors for up to 32 pin groups; changes sync live to desktop pins, top bar, and tray menu'
        )}
      >
        <div className="pin-group-settings">
          <div className="pin-group-settings__toolbar">
            <div className="pin-group-settings__hint">
              <span>{t('capture.pinGroupsShortcutGuide', 'Hover pin & press')}</span>
              <CodeBadge>Ctrl+1~9</CodeBadge>
              <span>{t('capture.pinGroupsShortcutAssign', 'to assign,')}</span>
              <CodeBadge>Alt+1~9</CodeBadge>
              <span>{t('capture.pinGroupsShortcutFilter', 'to filter by group')}</span>
            </div>
            <div className="pin-group-settings__toolbar-actions">
              {anyCustomized && (
                <Button
                  variant="ghost"
                  size="sm"
                  onClick={() => dispatchControl({ resetAllGroupMeta: true })}
                >
                  <RotateCcw size={14} aria-hidden="true" />
                  <span>{t('capture.pinGroupsResetAll', 'Reset All')}</span>
                </Button>
              )}
              <Button
                variant="secondary"
                size="sm"
                disabled={!canAddMore}
                onClick={handleAddNextSlot}
              >
                <Plus size={14} aria-hidden="true" />
                <span>
                  {t('capture.pinGroupsAddSlot', {
                    defaultValue: 'Add Group (Max {{max}})',
                    max: maxGroupId,
                  })}
                </span>
              </Button>
            </div>
          </div>

          <div className="pin-group-settings__list">
            {catalog.map((entry) => {
              const draft = draftNames[entry.id] ?? entry.customName ?? '';
              const isCustom =
                Boolean(entry.hasCustomization) ||
                Boolean(entry.customName && entry.customName.trim().length > 0) ||
                Boolean(entry.customColorHex && entry.customColorHex.trim().length > 0);
              const previewText = draft.trim() || entry.shortLabel;

              return (
                <div key={entry.id} className="pin-group-row">
                  <div className="pin-group-row__preview">
                    <span
                      className="pin-group-row__capsule"
                      style={{ borderColor: entry.colorHex }}
                      title={entry.fullLabel}
                    >
                      <span
                        className="pin-group-row__dot"
                        style={{ backgroundColor: entry.colorHex }}
                        aria-hidden="true"
                      />
                      <span className="pin-group-row__label">{previewText}</span>
                      {entry.count > 0 && (
                        <span className="pin-group-row__count">{entry.count}</span>
                      )}
                    </span>
                    {entry.id <= 9 && <CodeBadge variant="muted">{`Ctrl+${entry.id}`}</CodeBadge>}
                  </div>

                  <div className="pin-group-row__name-box">
                    <input
                      type="text"
                      className="pin-group-row__input"
                      value={draft}
                      maxLength={24}
                      aria-label={t('capture.pinGroupNameAria', {
                        defaultValue: 'Group {{id}} name',
                        id: entry.id,
                      })}
                      placeholder={t('capture.pinGroupNamePlaceholder', {
                        defaultValue: 'Custom name for Group {{id}} (e.g. Reference, UI Spec)...',
                        id: entry.id,
                      })}
                      onChange={(e) =>
                        setDraftNames((prev) => ({ ...prev, [entry.id]: e.target.value }))
                      }
                      onBlur={() => {
                        if (skipBlurIdRef.current === entry.id) {
                          skipBlurIdRef.current = null;
                          return;
                        }
                        handleCommitName(entry);
                      }}
                      onKeyDown={(e) => {
                        if (e.nativeEvent.isComposing || e.keyCode === 229) return;
                        if (e.key === 'Enter') {
                          e.preventDefault();
                          handleCommitName(entry);
                          e.currentTarget.blur();
                        } else if (e.key === 'Escape') {
                          e.preventDefault();
                          e.stopPropagation();
                          skipBlurIdRef.current = entry.id;
                          setDraftNames((prev) => ({
                            ...prev,
                            [entry.id]: entry.customName ?? '',
                          }));
                          try {
                            e.currentTarget.blur();
                          } finally {
                            skipBlurIdRef.current = null;
                          }
                        }
                      }}
                    />
                  </div>

                  <div className="pin-group-row__controls">
                    <div
                      className="pin-group-row__swatches"
                      role="radiogroup"
                      aria-label={t('capture.pinGroupColorAria', {
                        defaultValue: 'Group {{id}} preset colors',
                        id: entry.id,
                      })}
                    >
                      {presets.map((preset) => {
                        const active = preset.hex.toUpperCase() === entry.colorHex.toUpperCase();
                        return (
                          <button
                            key={preset.slot}
                            type="button"
                            role="radio"
                            aria-checked={active}
                            aria-label={`${preset.name} ${preset.hex}`}
                            title={`${preset.name} (${preset.hex})`}
                            className={`pin-group-swatch ${active ? 'pin-group-swatch--active' : ''}`}
                            style={{ backgroundColor: preset.hex }}
                            onClick={() => handleSelectColor(entry, preset.hex)}
                          />
                        );
                      })}
                    </div>

                    <label
                      className="pin-group-color-picker"
                      title={t('capture.pinGroupCustomColorTip', 'Pick any custom color')}
                    >
                      <Pipette size={13} aria-hidden="true" />
                      <span>{entry.colorHex.toUpperCase()}</span>
                      <input
                        type="color"
                        className="pin-group-color-picker__native"
                        value={entry.colorHex}
                        aria-label={t('capture.pinGroupCustomColorTip', 'Pick any custom color')}
                        onChange={(e) => handleSelectColor(entry, e.target.value)}
                      />
                    </label>

                    <button
                      type="button"
                      className="pin-group-row__reset-btn"
                      disabled={!isCustom}
                      title={t('capture.pinGroupResetOne', 'Restore default name and color')}
                      aria-label={t('capture.pinGroupResetOne', 'Restore default name and color')}
                      onClick={() => dispatchControl({ resetGroupMeta: entry.id })}
                    >
                      <RotateCcw size={14} aria-hidden="true" />
                    </button>
                  </div>
                </div>
              );
            })}
          </div>
        </div>
      </Card>
    </SettingGroup>
  );
};

/* ─────────────────────────────────────────────────────────────────────────────
 * ScopeRuleModal — 新增 / 编辑作用域规则 (复用共享 RunningWindowPicker)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, type FC } from 'react';
import { Modal, Field, Button, Select, TextInput, Tabs } from './UIKit';
import {
  type ScopeRule,
  MATCH_MODE_KEYS,
  emptyRule,
} from './scopeModel';
import {
  RunningWindowPicker,
  type WindowTargetKind,
  type PickedWindowResult,
} from './RunningWindowPicker';
import { useTranslation } from 'react-i18next';
import { Sparkles, Check, Ban, Cpu, Layers } from 'lucide-react';

interface Props {
  initial: ScopeRule | null;
  /** 可选的配置集名称, 供 effect===使用配置集 时选择 */
  profileNames: string[];
  onSave: (rule: ScopeRule) => void;
  onClose: () => void;
}

export const ScopeRuleModal: FC<Props> = ({ initial, profileNames, onSave, onClose }) => {
  const { t } = useTranslation();
  const [draft, setDraft] = useState<ScopeRule>(() => (initial ? structuredClone(initial) : emptyRule()));
  const [targetKind, setTargetKind] = useState<WindowTargetKind>(
    initial && initial.windowClass && !initial.processName ? 'class' : 'process',
  );
  const [justPicked, setJustPicked] = useState(false);

  const isEdit = initial !== null;
  const set = (patch: Partial<ScopeRule>) => setDraft((d) => ({ ...d, ...patch }));

  const targetValue = targetKind === 'process' ? draft.processName : draft.windowClass;
  const setTargetValue = (v: string) =>
    set(targetKind === 'process' ? { processName: v, windowClass: '' }
                                 : { windowClass: v, processName: '' });

  const handleWindowPick = (result: PickedWindowResult) => {
    const shouldUpdateName =
      result.source === 'foreground' || !draft.name || draft.name.startsWith('e.g.') || !isEdit;
    set({
      ...(shouldUpdateName ? { name: result.name } : {}),
      processName: targetKind === 'process' ? result.processName : '',
      windowClass: targetKind === 'class' ? result.windowClass : '',
    });
  };

  const nameError = draft.name.trim() ? '' : t('scope.nameRequired', 'Enter a rule name');
  const targetError = targetValue.trim() ? '' : t('scope.targetRequired', 'Enter a process or window class to match');
  const canSave = !nameError && !targetError;

  const handleSave = () => {
    if (!canSave) return;
    const rule: ScopeRule = {
      ...draft,
      name: draft.name.trim(),
      processName: targetKind === 'process' ? targetValue.trim() : '',
      windowClass: targetKind === 'class' ? targetValue.trim() : '',
      profileName: draft.effect === 2 ? (draft.profileName || (profileNames[0] ?? '')) : '',
    };
    onSave(rule);
  };

  return (
    <Modal
      open
      title={isEdit ? t('scope.editRule', 'Edit Scope Rule') : t('scope.newRule', 'New Scope Rule')}
      onClose={onClose}
      footer={
        <>
          <Button variant="ghost" onClick={onClose}>{t('common.cancel', 'Cancel')}</Button>
          <Button variant="primary" onClick={handleSave} disabled={!canSave}>{t('common.save', 'Save')}</Button>
        </>
      }
    >
      <RunningWindowPicker
        targetKind={targetKind}
        selectedProcess={draft.processName}
        selectedClass={draft.windowClass}
        fallbackPickedLabel={draft.name}
        onPick={handleWindowPick}
        onJustPickedChange={setJustPicked}
      />

      <div className={`app-picker-form ${justPicked ? 'just-picked-glow' : ''}`}>
        <Field label={t('scope.ruleName', 'Rule Name')} error={nameError}>
          <TextInput
            value={draft.name}
            onChange={(v) => set({ name: v })}
            placeholder={t('scope.ruleNamePlaceholder', 'e.g. Chrome browser')}
          />
        </Field>

        <Field label={t('scope.matchTarget', 'Match Target')}>
          <Tabs
            tabs={[
              { id: 'process', label: t('scope.byProcess', 'Process name'), icon: <Cpu size={14} aria-hidden="true" /> },
              { id: 'class', label: t('scope.byClass', 'Window class'), icon: <Layers size={14} aria-hidden="true" /> },
            ]}
            activeId={targetKind}
            onChange={(kind) => setTargetKind(kind as WindowTargetKind)}
            className="app-picker-kind-tabs-uikit"
          />
        </Field>

        <Field
          label={targetKind === 'process' ? t('scope.processName', 'Process Name') : t('scope.windowClass', 'Window Class')}
          error={targetError}
          hint={targetKind === 'process' ? t('scope.processHint', 'For example chrome.exe; wildcards such as *.exe are supported') : t('scope.classHint', 'For example Chrome_WidgetWin_1')}
        >
          <TextInput
            value={targetValue}
            onChange={setTargetValue}
            placeholder={targetKind === 'process' ? 'chrome.exe' : 'Chrome_WidgetWin_1'}
          />
        </Field>
      </div>

      <Field label={t('scope.matchMode', 'Match Mode')}>
        <Select
          value={String(draft.matchMode)}
          options={MATCH_MODE_KEYS.map((key, index) => ({ value: String(index), label: t(key) }))}
          onChange={(v) => set({ matchMode: Number(v) })}
          portal
        />
      </Field>

      <div className="uikit-field">
        <label className="uikit-field__label">{t('scope.effect', 'Effect')}</label>
        <div className="picker-strategy-cards">
          <div
            className={`picker-strategy-card ${draft.effect === 2 ? 'active' : ''}`}
            onClick={() => set({ effect: 2, profileName: draft.profileName || (profileNames[0] ?? '') })}
          >
            <div className="picker-strategy-card__icon">
              <Sparkles size={16} aria-hidden="true" />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('scope.effectProfile', 'Use profile')}</div>
              <div className="picker-strategy-card__desc">{t('scope.profileHint', 'Switch to this profile when the window matches')}</div>
            </div>
            {draft.effect === 2 && (
              <div className="picker-strategy-card__check">
                <Check size={11} aria-hidden="true" />
              </div>
            )}
          </div>

          <div
            className={`picker-strategy-card ${draft.effect === 1 ? 'active' : ''}`}
            onClick={() => set({ effect: 1 })}
          >
            <div className="picker-strategy-card__icon ban-icon">
              <Ban size={16} aria-hidden="true" />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('scope.effectDisable', 'Disable gestures')}</div>
              <div className="picker-strategy-card__desc">{t('components.disableGesturesInWindow', 'Completely disable mouse gestures in this window')}</div>
            </div>
            {draft.effect === 1 && (
              <div className="picker-strategy-card__check">
                <Check size={11} aria-hidden="true" />
              </div>
            )}
          </div>
        </div>
      </div>

      {draft.effect === 2 && (
        <Field label={t('scope.profile', 'Gesture Profile')} hint={t('scope.profileHint', 'Switch to this profile when the window matches')}>
          <Select
            value={draft.profileName || (profileNames[0] ?? '')}
            options={profileNames.map((n) => ({ value: n, label: n }))}
            onChange={(v) => set({ profileName: v })}
            portal
          />
        </Field>
      )}
    </Modal>
  );
};

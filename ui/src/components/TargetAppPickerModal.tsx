/* ─────────────────────────────────────────────────────────────────────────────
 * TargetAppPickerModal — 快速添加目标应用弹窗 (复用共享 RunningWindowPicker)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, type FC } from 'react';
import { Modal, Field, Button, TextInput, Tabs } from './UIKit';
import {
  RunningWindowPicker,
  type WindowTargetKind,
  type PickedWindowResult,
} from './RunningWindowPicker';
import { useTranslation } from 'react-i18next';
import { Sparkles, Check, Ban, Cpu, Layers } from 'lucide-react';

export interface AddedTargetResult {
  name: string;
  processName: string;
  windowClass: string;
  effect: number; // 1 = Disable, 2 = UseProfile
}

interface Props {
  defaultDisabled?: boolean;
  onAdd: (res: AddedTargetResult) => void;
  onClose: () => void;
}

export const TargetAppPickerModal: FC<Props> = ({ defaultDisabled = false, onAdd, onClose }) => {
  const { t } = useTranslation();
  const [name, setName] = useState('');
  const [targetKind, setTargetKind] = useState<WindowTargetKind>('process');
  const [targetValue, setTargetValue] = useState('');
  const [effect, setEffect] = useState<number>(defaultDisabled ? 1 : 2);
  const [justPicked, setJustPicked] = useState(false);

  const handleWindowPick = (result: PickedWindowResult) => {
    setName(result.name);
    setTargetValue(targetKind === 'process' ? result.processName : result.windowClass);
  };

  const nameError = name.trim() ? '' : t('scope.nameRequired', 'Enter a rule name');
  const targetError = targetValue.trim() ? '' : t('scope.targetRequired', 'Enter a process or window class to match');
  const canSave = !nameError && !targetError;

  const handleSave = () => {
    if (!canSave) return;
    onAdd({
      name: name.trim(),
      processName: targetKind === 'process' ? targetValue.trim() : '',
      windowClass: targetKind === 'class' ? targetValue.trim() : '',
      effect,
    });
  };

  return (
    <Modal
      open
      title={defaultDisabled ? t('picker.titleAddDisabled', 'Add Do Not Disturb Target (Blacklist)') : t('picker.titleAddApp', 'Add Target Application')}
      onClose={onClose}
      footer={
        <>
          <Button variant="ghost" onClick={onClose}>{t('common.cancel', 'Cancel')}</Button>
          <Button variant="primary" onClick={handleSave} disabled={!canSave}>{t('components.addAndConfigure', 'Add & Configure')}</Button>
        </>
      }
    >
      <RunningWindowPicker
        targetKind={targetKind}
        selectedProcess={targetKind === 'process' ? targetValue : ''}
        selectedClass={targetKind === 'class' ? targetValue : ''}
        fallbackPickedLabel={name}
        onPick={handleWindowPick}
        onJustPickedChange={setJustPicked}
      />

      <div className={`app-picker-form ${justPicked ? 'just-picked-glow' : ''}`}>
        <Field label={t('scope.ruleName', 'Rule Name')} error={nameError}>
          <TextInput
            value={name}
            onChange={setName}
            placeholder={t('components.appPlaceholder', 'e.g. Google Chrome / VS Code / Game')}
          />
        </Field>

        <Field label={t('scope.matchTarget', 'Match Target')}>
          <Tabs
            tabs={[
              { id: 'process', label: t('picker.kindProcess', 'By Process Name (Recommended, e.g. chrome.exe)'), icon: <Cpu size={14} aria-hidden="true" /> },
              { id: 'class', label: t('picker.kindClass', 'By Window Class Name (e.g. Chrome_WidgetWin_1)'), icon: <Layers size={14} aria-hidden="true" /> },
            ]}
            activeId={targetKind}
            onChange={(kind) => setTargetKind(kind as WindowTargetKind)}
            className="app-picker-kind-tabs-uikit"
          />
        </Field>

        <Field
          label={targetKind === 'process' ? t('picker.fieldProcess', 'Process Name') : t('picker.fieldClass', 'Window Class Name')}
          error={targetError}
          hint={targetKind === 'process' ? t('picker.hintProcess', 'Click from list above or enter manually (e.g. chrome.exe)') : t('picker.hintClass', 'Windows ClassName of target window')}
        >
          <TextInput
            value={targetValue}
            onChange={setTargetValue}
            placeholder={targetKind === 'process' ? 'chrome.exe' : 'Chrome_WidgetWin_1'}
          />
        </Field>
      </div>

      <div className="uikit-field">
        <label className="uikit-field__label">{t('components.scopeStrategyLabel', 'Scope Strategy')}</label>
        <div className="picker-strategy-cards">
          <div
            className={`picker-strategy-card ${effect === 2 ? 'active' : ''}`}
            onClick={() => setEffect(2)}
          >
            <div className="picker-strategy-card__icon">
              <Sparkles size={16} aria-hidden="true" />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('components.customIndependentTitle', 'Custom Independent Gestures')}</div>
              <div className="picker-strategy-card__desc">{t('components.customIndependentDesc', 'Override or extend global gestures')}</div>
            </div>
            {effect === 2 && (
              <div className="picker-strategy-card__check">
                <Check size={11} aria-hidden="true" />
              </div>
            )}
          </div>

          <div
            className={`picker-strategy-card ${effect === 1 ? 'active' : ''}`}
            onClick={() => setEffect(1)}
          >
            <div className="picker-strategy-card__icon ban-icon">
              <Ban size={16} aria-hidden="true" />
            </div>
            <div className="picker-strategy-card__content">
              <div className="picker-strategy-card__title">{t('components.disableGestureTitle', 'Disable Gestures (Do Not Disturb)')}</div>
              <div className="picker-strategy-card__desc">{t('components.disableGestureDesc', 'Completely disable gestures in this app')}</div>
            </div>
            {effect === 1 && (
              <div className="picker-strategy-card__check">
                <Check size={11} aria-hidden="true" />
              </div>
            )}
          </div>
        </div>
      </div>
    </Modal>
  );
};

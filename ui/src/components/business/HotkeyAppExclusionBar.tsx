/* ─────────────────────────────────────────────────────────────────────────────
 * HotkeyAppExclusionBar — 快捷键按前台应用自动让位配置栏 (SSOT 复用组件)
 *
 * 复用于:
 *   1. OcrPage.tsx (全局划词翻译 Alt+X 下方专属避让应用)
 *   2. GeneralPage.tsx (快捷键总览：全局热键免打扰名单 + 任意单热键专属避让弹窗)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { Plus, X, AppWindow, ShieldAlert } from 'lucide-react';
import { Button, Modal, TextInput } from '../UIKit';
import { RunningWindowPicker, type PickedWindowResult } from '../RunningWindowPicker';
import { normalizeProcessInput } from '../../utils/processName';
import './HotkeyAppExclusionBar.css';

export interface HotkeyAppExclusionBarProps {
  apps: string[];
  onChange: (nextApps: string[]) => void;
  disabled?: boolean;
  compact?: boolean;
  emptyText?: string;
  pickerTitle?: string;
}

export const HotkeyAppExclusionBar: FC<HotkeyAppExclusionBarProps> = ({
  apps,
  onChange,
  disabled = false,
  compact = false,
  emptyText,
  pickerTitle,
}) => {
  const { t } = useTranslation();
  const [draft, setDraft] = useState('');
  const [pickerOpen, setPickerOpen] = useState(false);
  const [pickedProcess, setPickedProcess] = useState('');

  const addProcess = (raw: string) => {
    const normalized = normalizeProcessInput(raw);
    if (!normalized) return;
    if (apps.some((item) => item.toLowerCase() === normalized)) {
      setDraft('');
      return;
    }
    onChange([...apps, normalized]);
    setDraft('');
  };

  const removeProcess = (proc: string) => {
    onChange(apps.filter((item) => item.toLowerCase() !== proc.toLowerCase()));
  };

  const handleWindowPick = (res: PickedWindowResult) => {
    setPickedProcess(res.processName);
  };

  const handleConfirmPick = () => {
    if (pickedProcess.trim()) {
      addProcess(pickedProcess);
    }
    setPickedProcess('');
    setPickerOpen(false);
  };

  return (
    <div className={`hotkey-exclusion-bar ${compact ? 'hotkey-exclusion-bar--compact' : ''}`}>
      <div className="hotkey-exclusion-bar__chips" role="list">
        {apps.length === 0 ? (
          <span className="hotkey-exclusion-bar__empty">
            <ShieldAlert size={13} aria-hidden="true" />
            <span>{emptyText || t('hotkeyExclusion.emptyHint', 'No excluded apps yet')}</span>
          </span>
        ) : (
          apps.map((app) => (
            <span key={app} className="hotkey-exclusion-bar__chip" role="listitem">
              <span className="hotkey-exclusion-bar__chip-name">{app}</span>
              <button
                type="button"
                className="hotkey-exclusion-bar__chip-remove"
                disabled={disabled}
                onClick={() => removeProcess(app)}
                aria-label={t('hotkeyExclusion.removeAppAria', 'Remove {{app}}', { app })}
                title={t('hotkeyExclusion.removeAppAria', 'Remove {{app}}', { app })}
              >
                <X size={12} aria-hidden="true" />
              </button>
            </span>
          ))
        )}
      </div>

      <div className="hotkey-exclusion-bar__controls">
        <div
          className="hotkey-exclusion-bar__input-wrap"
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              e.preventDefault();
              addProcess(draft);
            }
          }}
        >
          <TextInput
            value={draft}
            onChange={setDraft}
            disabled={disabled}
            placeholder={t('hotkeyExclusion.inputPlaceholder', 'e.g. ssms.exe, emacs.exe')}
          />
        </div>
        <Button
          variant="secondary"
          size="sm"
          disabled={disabled || !draft.trim()}
          onClick={() => addProcess(draft)}
        >
          <Plus size={14} aria-hidden="true" />
          <span>{t('hotkeyExclusion.addBtn', 'Add')}</span>
        </Button>
        <Button
          variant="ghost"
          size="sm"
          disabled={disabled}
          onClick={() => {
            setPickedProcess('');
            setPickerOpen(true);
          }}
        >
          <AppWindow size={14} aria-hidden="true" />
          <span>{t('hotkeyExclusion.pickWindowBtn', 'Pick Running App')}</span>
        </Button>
      </div>

      {pickerOpen && (
        <Modal
          open
          title={pickerTitle || t('hotkeyExclusion.pickerTitle', 'Pick Application to Auto-Yield Hotkey')}
          onClose={() => setPickerOpen(false)}
          footer={
            <>
              <Button variant="ghost" onClick={() => setPickerOpen(false)}>
                {t('common.cancel', 'Cancel')}
              </Button>
              <Button
                variant="primary"
                disabled={!pickedProcess.trim()}
                onClick={handleConfirmPick}
              >
                {t('hotkeyExclusion.addBtn', 'Add')}
              </Button>
            </>
          }
        >
          <RunningWindowPicker
            targetKind="process"
            selectedProcess={pickedProcess}
            selectedClass=""
            fallbackPickedLabel=""
            onPick={handleWindowPick}
          />
          <div className="hotkey-exclusion-bar__picker-preview">
            <TextInput
              value={pickedProcess}
              onChange={setPickedProcess}
              placeholder={t('hotkeyExclusion.inputPlaceholder', 'e.g. ssms.exe, emacs.exe')}
            />
          </div>
        </Modal>
      )}
    </div>
  );
};

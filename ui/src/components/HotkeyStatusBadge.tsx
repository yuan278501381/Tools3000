import type { FC } from 'react';
import { AlertTriangle, AlertOctagon } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import './HotkeyStatusBadge.css';

export interface HotkeyEntry {
  name: string;
  shortcut: string;
  registered?: boolean;
  armed?: boolean;
  scope?: 'global' | 'recording' | 'dialog' | string;
  conflict?: boolean;
  conflictType?: 'none' | 'internal' | 'external' | 'invalid' | string;
  conflictWith?: string;
}

interface HotkeyStatusBadgeProps {
  entry?: HotkeyEntry;
  shortcut?: string;
  registered?: boolean;
  conflictType?: string;
  conflictWith?: string;
}

export const HotkeyStatusBadge: FC<HotkeyStatusBadgeProps> = ({
  entry,
  shortcut: customShortcut,
  registered: customRegistered,
  conflictType: customConflictType,
  conflictWith: customConflictWith,
}) => {
  const { t } = useTranslation();

  const shortcut = entry?.shortcut ?? customShortcut ?? '';
  const registered = entry?.registered ?? customRegistered;
  const conflictType = entry?.conflictType ?? customConflictType;
  const conflictWith = entry?.conflictWith ?? customConflictWith;

  if (!shortcut) {
    return (
      <span className="hotkey-badge badge-disabled">
        <span className="hotkey-badge__dot" aria-hidden="true" />
        <span>{t('general.shortcutDisabled', 'Disabled')}</span>
      </span>
    );
  }

  const armed = entry?.armed !== false;
  const isInternal = conflictType === 'internal';
  const isExternal = conflictType === 'external' ||
    (registered === false && Boolean(shortcut) && armed);
  const isSessionOnly = Boolean(shortcut) && !armed && !isInternal && conflictType !== 'external';
  const isActive = (registered !== false) && armed && !entry?.conflict && !isInternal && !isExternal;

  if (isSessionOnly) {
    const isDialogScope = entry?.scope === 'dialog';
    const scopedLabel = isDialogScope
      ? t('general.shortcutDialogOnly', 'Active in file dialogs')
      : t('general.shortcutRecordingOnly', 'Active while recording');
    return (
      <span className="hotkey-badge badge-session" title={scopedLabel}>
        <span className="hotkey-badge__dot" aria-hidden="true" />
        <span>{scopedLabel}</span>
      </span>
    );
  }

  if (isInternal) {
    return (
      <span className="hotkey-badge badge-warning" title={conflictWith || t('general.shortcutConflictInternal', 'Internal Conflict: duplicate with other plugin shortcut')}>
        <AlertTriangle size={12} aria-hidden="true" />
        <span>{conflictWith || t('general.shortcutConflictInternal', 'Internal Conflict: duplicate with other plugin shortcut')}</span>
      </span>
    );
  }

  if (isExternal) {
    return (
      <span className="hotkey-badge badge-danger" title={conflictWith || t('general.shortcutConflictExternal', 'External Conflict: occupied by system or 3rd-party app')}>
        <AlertOctagon size={12} aria-hidden="true" />
        <span>{conflictWith || t('general.shortcutConflictExternal', 'External Conflict: occupied by system or 3rd-party app')}</span>
      </span>
    );
  }

  if (isActive) {
    return (
      <span className="hotkey-badge badge-active" title={t('components.hotkeyActiveTip', 'Hotkey registered and active globally')}>
        <span className="hotkey-badge__dot" aria-hidden="true" />
        <span>{t('general.shortcutActive', 'Active')}</span>
      </span>
    );
  }

  return null;
};

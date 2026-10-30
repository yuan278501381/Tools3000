import type { FC } from 'react';
import { CheckCircle2, AlertTriangle, AlertOctagon, Disc, MinusCircle } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import './HotkeyStatusBadge.css';

export interface HotkeyEntry {
  name: string;
  shortcut: string;
  registered?: boolean;
  armed?: boolean;
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
        <MinusCircle size={11} />
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
    return (
      <span className="hotkey-badge badge-session" title={t('general.shortcutRecordingOnly', 'Active while recording')}>
        <Disc size={11} />
        <span>{t('general.shortcutRecordingOnly', 'Active while recording')}</span>
      </span>
    );
  }

  if (isInternal) {
    return (
      <span className="hotkey-badge badge-warning" title={conflictWith || t('general.shortcutConflictInternal', 'Internal Conflict: duplicate with other plugin shortcut')}>
        <AlertTriangle size={11} />
        <span>{conflictWith || t('general.shortcutConflictInternal', 'Internal Conflict: duplicate with other plugin shortcut')}</span>
      </span>
    );
  }

  if (isExternal) {
    return (
      <span className="hotkey-badge badge-danger" title={conflictWith || t('general.shortcutConflictExternal', 'External Conflict: occupied by system or 3rd-party app')}>
        <AlertOctagon size={11} />
        <span>{conflictWith || t('general.shortcutConflictExternal', 'External Conflict: occupied by system or 3rd-party app')}</span>
      </span>
    );
  }

  if (isActive) {
    return (
      <span className="hotkey-badge badge-active" title={t('components.hotkeyActiveTip', 'Hotkey registered and active globally')}>
        <CheckCircle2 size={11} />
        <span>{t('general.shortcutActive', 'Active')}</span>
      </span>
    );
  }

  return null;
};

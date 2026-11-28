/* ─────────────────────────────────────────────────────────────────────────────
 * HotkeyRecorder — 交互式快捷键录入器
 *
 * 功能:
 *   - 点击进入录入模式（边框高亮 + 提示文字 "按下快捷键..."）
 *   - 实时监听键盘事件，显示当前按下的键组合
 *   - 支持 Ctrl / Alt / Shift / Win + 任意键
 *   - 每个修饰键用独立的圆角胶囊渲染（macOS 偏好风格）
 *   - Esc 取消录入，Backspace 清除当前绑定
 *   - 录入完成（松开所有键）后自动确认
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useCallback, useRef, useEffect, type FC, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { X, AlertCircle } from 'lucide-react';
import { useControlA11y } from './ControlA11yContext';
import { acquireHotkeyPause, releaseHotkeyPause } from '../utils/hotkeyPauseCoordinator';
import './HotkeyRecorder.css';

/** 修饰键集合 */
const MODIFIER_KEYS = new Set(['Control', 'Alt', 'Shift', 'Meta', 'AltGraph', 'OS']);

/** 支持双击触发的单一修饰键白名单（严禁 Win 键双击以免触发系统开始菜单） */
const DOUBLE_TAP_MODIFIERS = new Set(['Ctrl', 'Alt', 'Shift']);
const DOUBLE_TAP_MAX_HOLD_MS = 280;
const DOUBLE_TAP_MAX_GAP_MS = 400;

const parseDoubleTapModifier = (raw: string): string | null => {
  const m = /^Double\+(Ctrl|Alt|Shift)$/i.exec(raw.trim());
  if (!m) return null;
  const lower = m[1].toLowerCase();
  if (lower === 'ctrl') return 'Ctrl';
  if (lower === 'alt') return 'Alt';
  return 'Shift';
};

/** 全键位标准化解析 */
const normalizeKey = (e: KeyboardEvent): string => {
  // 1. 空格键
  if (e.code === 'Space' || e.key === ' ' || e.key === 'Spacebar') return 'Space';
  // 2. 小键盘键区
  if (e.code.startsWith('Numpad')) {
    const num = e.code.replace('Numpad', '');
    if (/^\d$/.test(num)) return `Num${num}`;
    const numpadMap: Record<string, string> = {
      Add: 'Num+', Subtract: 'Num-', Multiply: 'Num*', Divide: 'Num/', Decimal: 'Num.', Enter: 'NumEnter'
    };
    return numpadMap[num] || `Num${num}`;
  }
  // 3. 功能键 F1 ~ F24
  if (/^F\d{1,2}$/i.test(e.key)) return e.key.toUpperCase();
  // 4. 特殊导航与编辑键
  const keyMap: Record<string, string> = {
    ArrowLeft: 'Left', ArrowRight: 'Right', ArrowUp: 'Up', ArrowDown: 'Down',
    Enter: 'Enter', Tab: 'Tab', Backspace: 'Backspace', Delete: 'Delete', Home: 'Home', End: 'End',
    PageUp: 'PageUp', PageDown: 'PageDown', Insert: 'Insert', CapsLock: 'CapsLock',
    PrintScreen: 'PrintScreen', ScrollLock: 'ScrollLock', Pause: 'Pause',
    ContextMenu: 'Apps',
    AudioVolumeMute: 'VolumeMute', VolumeMute: 'VolumeMute',
    AudioVolumeDown: 'VolumeDown', VolumeDown: 'VolumeDown',
    AudioVolumeUp: 'VolumeUp', VolumeUp: 'VolumeUp',
    MediaTrackNext: 'MediaNextTrack', MediaNextTrack: 'MediaNextTrack',
    MediaTrackPrevious: 'MediaPrevTrack', MediaPreviousTrack: 'MediaPrevTrack', MediaPrevTrack: 'MediaPrevTrack',
    MediaStop: 'MediaStop',
    MediaPlayPause: 'MediaPlayPause',
  };
  if (keyMap[e.key]) return keyMap[e.key];
  if (keyMap[e.code]) return keyMap[e.code];
  // 5. 符号键映射
  const symbolMap: Record<string, string> = {
    '`': '`', '~': '`',
    '-': '-', '_': '-',
    '=': '=', '+': '=',
    '[': '[', '{': '[',
    ']': ']', '}': ']',
    '\\': '\\', '|': '\\',
    ';': ';', ':': ';',
    "'": "'", '"': "'",
    ',': ',', '<': ',',
    '.': '.', '>': '.',
    '/': '/', '?': '/',
  };
  if (symbolMap[e.key]) return symbolMap[e.key];
  // 6. 单字符/字母
  if (e.key.length === 1) return e.key.toUpperCase();
  return e.key;
};

/** 验证按键组合安全性：单字母、单数字或单标点符号禁止作为全局热键，必须包含 Ctrl、Alt 或 Win */
const isSafeKeyCombo = (mods: string[], mainKey: string): boolean => {
  if (mods.includes('Ctrl') || mods.includes('Alt') || mods.includes('Win')) {
    return true;
  }
  // 放行功能键 F1 ~ F24
  if (/^F\d{1,2}$/i.test(mainKey)) return true;
  // 放行独立控制键
  if (mainKey === 'PrintScreen' || mainKey === 'ScrollLock' || mainKey === 'Pause') return true;
  // 放行小键盘独立算符
  if (['Num+', 'Num-', 'Num*', 'Num/', 'Num.'].includes(mainKey)) return true;
  // 放行多媒体按键
  if (['VolumeMute', 'VolumeDown', 'VolumeUp', 'MediaNextTrack', 'MediaPrevTrack', 'MediaStop', 'MediaPlayPause'].includes(mainKey)) return true;
  return false;
};

interface HotkeyRecorderProps {
  /** 当前快捷键值, 格式如 "Ctrl+Shift+A" */
  value: string;
  /** 快捷键变更回调 */
  onChange: (value: string) => void;
  /** 未绑定时的占位文字 */
  placeholder?: string;
  /** 唯一标识 */
  id: string;
  ariaLabel?: string;
  /** 是否禁用录入与清空 */
  disabled?: boolean;
  /**
   * 允许录入单个按键（如 Delete / Backspace / Enter / 方向键等），专用于手势发送按键等
   * 目标窗口动作场景；默认 false 保持全局热键防误触校验。
   */
  allowSingleKey?: boolean;
  /**
   * 允许录入双击同一修饰键（Double+Ctrl / Double+Alt / Double+Shift），
   * 全局快捷键模式默认开启，手势发送单键模式默认关闭。
   */
  allowDoubleTapModifier?: boolean;
}

interface DoubleTapTrackState {
  mod: string | null;
  phase: 'idle' | 'first_down' | 'waiting_second';
  firstDownAt: number;
  firstUpAt: number;
}

export const HotkeyRecorder: FC<HotkeyRecorderProps> = ({
  value,
  onChange,
  placeholder,
  id,
  ariaLabel,
  disabled = false,
  allowSingleKey = false,
  allowDoubleTapModifier,
}) => {
  const doubleTapAllowed = allowDoubleTapModifier ?? !allowSingleKey;
  const { t } = useTranslation();
  const a11y = useControlA11y();
  const [recording, setRecording] = useState(false);
  const [activeKeys, setActiveKeys] = useState<string[]>([]);
  const [waitingSecondTapMod, setWaitingSecondTapMod] = useState<string | null>(null);
  const [warningTip, setWarningTip] = useState(false);
  const containerRef = useRef<HTMLDivElement>(null);
  const recordingRef = useRef(false);
  const holdsPauseLockRef = useRef(false);
  const pendingRef = useRef<string | null>(null);
  const doubleTapRef = useRef<DoubleTapTrackState>({ mod: null, phase: 'idle', firstDownAt: 0, firstUpAt: 0 });
  const doubleTapTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const warningTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const lockHotkeyPause = useCallback(() => {
    if (!holdsPauseLockRef.current) {
      holdsPauseLockRef.current = true;
      acquireHotkeyPause();
    }
  }, []);

  const unlockHotkeyPause = useCallback(() => {
    if (holdsPauseLockRef.current) {
      holdsPauseLockRef.current = false;
      releaseHotkeyPause();
    }
  }, []);

  const clearWarning = useCallback(() => {
    if (warningTimerRef.current) {
      clearTimeout(warningTimerRef.current);
      warningTimerRef.current = null;
    }
    setWarningTip(false);
  }, []);

  const clearDoubleTapState = useCallback(() => {
    if (doubleTapTimerRef.current) {
      clearTimeout(doubleTapTimerRef.current);
      doubleTapTimerRef.current = null;
    }
    doubleTapRef.current = { mod: null, phase: 'idle', firstDownAt: 0, firstUpAt: 0 };
    setWaitingSecondTapMod(null);
  }, []);

  // ── 进入录入模式 ──────────────────────────────────────────────────────────
  const startRecording = useCallback(() => {
    if (disabled) return;
    recordingRef.current = true;
    setRecording(true);
    setActiveKeys([]);
    pendingRef.current = null;
    clearDoubleTapState();
    clearWarning();
    lockHotkeyPause();
  }, [clearDoubleTapState, clearWarning, disabled, lockHotkeyPause]);

  // ── 退出录入模式 (不保存) ─────────────────────────────────────────────────
  const cancelRecording = useCallback(() => {
    recordingRef.current = false;
    setRecording(false);
    setActiveKeys([]);
    pendingRef.current = null;
    clearDoubleTapState();
    clearWarning();
    unlockHotkeyPause();
  }, [clearDoubleTapState, clearWarning, unlockHotkeyPause]);

  // ── 确认录入结果 ──────────────────────────────────────────────────────────
  const confirmRecording = useCallback((hotkeyStr: string) => {
    recordingRef.current = false;
    setRecording(false);
    setActiveKeys([]);
    clearDoubleTapState();
    clearWarning();
    if (hotkeyStr && hotkeyStr !== value) {
      onChange(hotkeyStr);
    }
    pendingRef.current = null;
    unlockHotkeyPause();
  }, [clearDoubleTapState, clearWarning, onChange, unlockHotkeyPause, value]);

  // 组件卸载时若仍处于录入状态则确保恢复全局热键引用计数
  useEffect(() => {
    return () => {
      clearDoubleTapState();
      clearWarning();
      recordingRef.current = false;
      unlockHotkeyPause();
    };
  }, [clearDoubleTapState, clearWarning, unlockHotkeyPause]);

  // 父级级联禁用时自动退出录制态并归还全局热键暂停锁
  useEffect(() => {
    if (disabled && recordingRef.current) {
      cancelRecording();
    }
  }, [disabled, cancelRecording]);

  // ── 键盘事件监听 ──────────────────────────────────────────────────────────
  useEffect(() => {
    if (!recording) return;

    const handleKeyDown = (e: KeyboardEvent) => {
      e.preventDefault();
      e.stopPropagation();

      const key = e.key;

      // Esc 取消 (仅在未按下任何修饰键时生效，避免与 Ctrl+Esc 等快捷键冲突)
      if (key === 'Escape' && !e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
        cancelRecording();
        return;
      }

      // 全局热键模式下 Backspace 清除绑定；手势发送单键模式下 Backspace 作为合法可录入按键
      if (!allowSingleKey && key === 'Backspace' && !e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
        onChange('');
        recordingRef.current = false;
        setRecording(false);
        setActiveKeys([]);
        pendingRef.current = null;
        clearWarning();
        unlockHotkeyPause();
        return;
      }

      // 收集当前修饰键
      const mods: string[] = [];
      if (e.ctrlKey) mods.push('Ctrl');
      if (e.altKey) mods.push('Alt');
      if (e.shiftKey) mods.push('Shift');
      if (e.metaKey) mods.push('Win');

      const isSpace = e.code === 'Space' || key === ' ' || key === 'Spacebar' || e.keyCode === 32;

      if (MODIFIER_KEYS.has(key) && !isSpace) {
        // 仅按下了修饰键：若开启修饰键双击录制且为单一白名单修饰键，跟踪 Down -> Up -> Down 闭环
        clearWarning();
        if (doubleTapAllowed && mods.length === 1 && DOUBLE_TAP_MODIFIERS.has(mods[0])) {
          const mod = mods[0];
          const now = Date.now();
          if (e.repeat) {
            if (
              doubleTapRef.current.phase === 'first_down' &&
              now - doubleTapRef.current.firstDownAt > DOUBLE_TAP_MAX_HOLD_MS
            ) {
              clearDoubleTapState();
            }
            setActiveKeys([...mods]);
            return;
          }
          if (
            doubleTapRef.current.phase === 'waiting_second' &&
            doubleTapRef.current.mod === mod &&
            now - doubleTapRef.current.firstUpAt <= DOUBLE_TAP_MAX_GAP_MS
          ) {
            const comboStr = `Double+${mod}`;
            clearDoubleTapState();
            setActiveKeys(['Double', mod]);
            pendingRef.current = comboStr;
            return;
          }
          if (doubleTapTimerRef.current) {
            clearTimeout(doubleTapTimerRef.current);
            doubleTapTimerRef.current = null;
          }
          setWaitingSecondTapMod(null);
          doubleTapRef.current = { mod, phase: 'first_down', firstDownAt: now, firstUpAt: 0 };
          setActiveKeys([...mods]);
          pendingRef.current = null;
        } else {
          clearDoubleTapState();
          setActiveKeys([...mods]);
          pendingRef.current = null;
        }
      } else {
        // 修饰键 + 主键 → 完成组合（立即中止修饰键双击候选）
        clearDoubleTapState();
        const mainKey = normalizeKey(e);
        const filteredMods = mods.filter(m => m !== mainKey);
        const combo = [...filteredMods, mainKey];
        setActiveKeys(combo);

        if (allowSingleKey || isSafeKeyCombo(filteredMods, mainKey)) {
          clearWarning();
          pendingRef.current = combo.join('+');
        } else {
          // 不安全的单键（如单字母、单数字、单符号），提示用户配合修饰键
          pendingRef.current = null;
          setWarningTip(true);
          if (warningTimerRef.current) clearTimeout(warningTimerRef.current);
          warningTimerRef.current = setTimeout(() => {
            setWarningTip(false);
          }, 3000);
        }
      }
    };

    const handleKeyUp = (e: KeyboardEvent) => {
      // 当有挂起的合法组合键或双击修饰键时，在松开任意键后确认
      if (pendingRef.current) {
        confirmRecording(pendingRef.current);
        return;
      }
      // 检测首次修饰键轻触释放：若按住 <= 280ms 则进入等待第二次轻击窗口 (400ms)
      if (
        doubleTapAllowed &&
        doubleTapRef.current.phase === 'first_down' &&
        doubleTapRef.current.mod &&
        !e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey
      ) {
        const now = Date.now();
        const mod = doubleTapRef.current.mod;
        if (now - doubleTapRef.current.firstDownAt <= DOUBLE_TAP_MAX_HOLD_MS) {
          doubleTapRef.current = { mod, phase: 'waiting_second', firstDownAt: doubleTapRef.current.firstDownAt, firstUpAt: now };
          setWaitingSecondTapMod(mod);
          setActiveKeys([]);
          if (doubleTapTimerRef.current) clearTimeout(doubleTapTimerRef.current);
          doubleTapTimerRef.current = setTimeout(() => {
            doubleTapRef.current = { mod: null, phase: 'idle', firstDownAt: 0, firstUpAt: 0 };
            setWaitingSecondTapMod(null);
          }, DOUBLE_TAP_MAX_GAP_MS);
          return;
        }
        clearDoubleTapState();
      }
      // 如果未构成合法组合且所有修饰键均已松开，重置活跃按键显示
      if (!e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
        setActiveKeys([]);
      }
    };

    window.addEventListener('keydown', handleKeyDown, true);
    window.addEventListener('keyup', handleKeyUp, true);

    return () => {
      window.removeEventListener('keydown', handleKeyDown, true);
      window.removeEventListener('keyup', handleKeyUp, true);
    };
  }, [recording, allowSingleKey, doubleTapAllowed, cancelRecording, clearDoubleTapState, confirmRecording, onChange, clearWarning, unlockHotkeyPause]);

  // ── 点击外部退出录入 ──────────────────────────────────────────────────────
  useEffect(() => {
    if (!recording) return;

    const handleClickOutside = (e: MouseEvent) => {
      if (containerRef.current && !containerRef.current.contains(e.target as Node)) {
        cancelRecording();
      }
    };

    const timer = setTimeout(() => {
      document.addEventListener('mousedown', handleClickOutside);
    }, 0);

    return () => {
      clearTimeout(timer);
      document.removeEventListener('mousedown', handleClickOutside);
    };
  }, [recording, cancelRecording]);

  // ── 解析已绑定的快捷键字符串为 token 列表 ─────────────────────────────────
  const parsedDoubleMod = parseDoubleTapModifier(value || '');
  const parsedKeys = parsedDoubleMod ? ['Double', parsedDoubleMod] : (value ? value.split('+') : []);

  // ── 渲染物理键帽簇 ────────────────────────────────────────────────────────
  const renderKeys = (keys: string[]) => {
    if (keys.length === 0) return null;

    if (keys.length === 2 && keys[0] === 'Double' && DOUBLE_TAP_MODIFIERS.has(keys[1])) {
      return (
        <span className="hotkey-recorder__keys">
          <kbd className="hotkey-recorder__key">
            {t('hotkey.doubleTapDisplay', 'Double {{key}}', { key: keys[1] })}
          </kbd>
        </span>
      );
    }

    return (
      <span className="hotkey-recorder__keys">
        {keys.map((key, idx) => (
          <span key={`${key}-${idx}`} style={{ display: 'inline-flex', alignItems: 'center' }}>
            {idx > 0 && <span className="hotkey-recorder__separator">+</span>}
            <kbd className="hotkey-recorder__key">{key}</kbd>
          </span>
        ))}
      </span>
    );
  };

  const defaultPlaceholder = placeholder ?? t('capture.pressToBind', 'Press any key combination...');
  const recordingPlaceholder = doubleTapAllowed
    ? t('hotkey.pressKeysOrDoubleTap', 'Press combo or double-tap Ctrl/Alt/Shift...')
    : t('hotkey.pressKeys', 'Press a key or combination...');
  const formattedAccessibleValue = parsedDoubleMod
    ? t('hotkey.doubleTapDisplay', 'Double {{key}}', { key: parsedDoubleMod })
    : (value || defaultPlaceholder);

  return (
    <div ref={containerRef} className="hotkey-recorder-wrap">
      <button
        id={id}
        type="button"
        disabled={disabled}
        className={`hotkey-recorder ${recording ? 'hotkey-recorder--recording' : ''}`}
        onClick={() => !disabled && !recording && startRecording()}
        onKeyDown={(e: ReactKeyboardEvent) => {
          if (!disabled && !recording && (e.key === 'Enter' || e.key === ' ')) {
            e.preventDefault();
            startRecording();
            return;
          }
          if (recording && e.key === 'Tab') e.preventDefault();
        }}
        aria-pressed={recording}
        aria-labelledby={!ariaLabel && a11y.labelledBy ? `${a11y.labelledBy} ${id}-value` : undefined}
        aria-describedby={a11y.describedBy}
        aria-label={ariaLabel
          ? `${ariaLabel}: ${recording ? recordingPlaceholder : formattedAccessibleValue}`
          : a11y.labelledBy ? undefined : formattedAccessibleValue}
      >
        <span id={`${id}-value`} className="sr-only">
          {recording ? recordingPlaceholder : formattedAccessibleValue}
        </span>
        <span aria-hidden="true" style={{ display: 'inline-flex', alignItems: 'center' }}>
          {recording ? (
            activeKeys.length > 0 ? renderKeys(activeKeys) : waitingSecondTapMod ? (
              <span className="hotkey-recorder__placeholder">
                {t('hotkey.tapAgainToDouble', 'Tap {{key}} again...', { key: waitingSecondTapMod })}
              </span>
            ) : (
              <span className="hotkey-recorder__placeholder">{recordingPlaceholder}</span>
            )
          ) : (
            parsedKeys.length > 0 ? renderKeys(parsedKeys) : (
              <span className="hotkey-recorder__placeholder">{defaultPlaceholder}</span>
            )
          )}
        </span>
      </button>
      {value && !recording && !disabled && (
        <button
          type="button"
          className="hotkey-recorder__clear"
          onClick={() => onChange('')}
          aria-label={t('hotkey.clearBinding', 'Clear hotkey binding')}
          title={t('hotkey.clearBinding', 'Clear hotkey binding')}
        >
          <X size={12} strokeWidth={2.5} />
        </button>
      )}
      {recording && warningTip && (
        <div className="hotkey-recorder__tip" role="alert">
          <AlertCircle size={13} className="hotkey-recorder__tip-icon" />
          <span>{t('hotkey.needModifier', 'Letters and symbols must be used with Ctrl, Alt, or Win')}</span>
        </div>
      )}
    </div>
  );
};

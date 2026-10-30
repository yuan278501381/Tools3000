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
import { bridgeRequest } from '../hooks/useBridge';
import './HotkeyRecorder.css';

/** 修饰键集合 */
const MODIFIER_KEYS = new Set(['Control', 'Alt', 'Shift', 'Meta', 'AltGraph', 'OS']);

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
    Enter: 'Enter', Tab: 'Tab', Delete: 'Delete', Home: 'Home', End: 'End',
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
}

export const HotkeyRecorder: FC<HotkeyRecorderProps> = ({ value, onChange, placeholder, id, ariaLabel }) => {
  const { t } = useTranslation();
  const a11y = useControlA11y();
  const [recording, setRecording] = useState(false);
  const [activeKeys, setActiveKeys] = useState<string[]>([]);
  const [warningTip, setWarningTip] = useState(false);
  const containerRef = useRef<HTMLDivElement>(null);
  const recordingRef = useRef(false);
  const pendingRef = useRef<string | null>(null);
  const warningTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const clearWarning = useCallback(() => {
    if (warningTimerRef.current) {
      clearTimeout(warningTimerRef.current);
      warningTimerRef.current = null;
    }
    setWarningTip(false);
  }, []);

  // ── 进入录入模式 ──────────────────────────────────────────────────────────
  const startRecording = useCallback(() => {
    recordingRef.current = true;
    setRecording(true);
    setActiveKeys([]);
    pendingRef.current = null;
    clearWarning();
    bridgeRequest('hotkey.setPaused', { paused: true }).catch(() => {});
  }, [clearWarning]);

  // ── 退出录入模式 (不保存) ─────────────────────────────────────────────────
  const cancelRecording = useCallback(() => {
    const wasRecording = recordingRef.current;
    recordingRef.current = false;
    setRecording(false);
    setActiveKeys([]);
    pendingRef.current = null;
    clearWarning();
    if (wasRecording) {
      bridgeRequest('hotkey.setPaused', { paused: false }).catch(() => {});
    }
  }, [clearWarning]);

  // ── 确认录入结果 ──────────────────────────────────────────────────────────
  const confirmRecording = useCallback((hotkeyStr: string) => {
    const wasRecording = recordingRef.current;
    recordingRef.current = false;
    setRecording(false);
    setActiveKeys([]);
    clearWarning();
    if (hotkeyStr && hotkeyStr !== value) {
      onChange(hotkeyStr);
    }
    pendingRef.current = null;
    if (wasRecording) {
      bridgeRequest('hotkey.setPaused', { paused: false }).catch(() => {});
    }
  }, [clearWarning, onChange, value]);

  // 组件卸载时若仍处于录入状态则确保恢复全局热键
  useEffect(() => {
    return () => {
      clearWarning();
      if (recordingRef.current) {
        recordingRef.current = false;
        bridgeRequest('hotkey.setPaused', { paused: false }).catch(() => {});
      }
    };
  }, [clearWarning]);

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

      // Backspace 清除绑定 (仅在未按下任何修饰键时生效)
      if (key === 'Backspace' && !e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
        onChange('');
        recordingRef.current = false;
        setRecording(false);
        setActiveKeys([]);
        pendingRef.current = null;
        clearWarning();
        bridgeRequest('hotkey.setPaused', { paused: false }).catch(() => {});
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
        // 仅按下了修饰键，实时展示
        setActiveKeys([...mods]);
        pendingRef.current = null;
        clearWarning();
      } else {
        // 修饰键 + 主键 → 完成组合
        const mainKey = normalizeKey(e);
        const filteredMods = mods.filter(m => m !== mainKey);
        const combo = [...filteredMods, mainKey];
        setActiveKeys(combo);

        if (isSafeKeyCombo(filteredMods, mainKey)) {
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
      // 当有挂起的合法组合键时，在松开任意键后确认
      if (pendingRef.current) {
        confirmRecording(pendingRef.current);
        return;
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
  }, [recording, cancelRecording, confirmRecording, onChange, clearWarning]);

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
  const parsedKeys = value ? value.split('+') : [];

  // ── 渲染物理键帽簇 ────────────────────────────────────────────────────────
  const renderKeys = (keys: string[]) => {
    if (keys.length === 0) return null;

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
  const recordingPlaceholder = t('hotkey.pressKeys', 'Press a key or combination...');

  return (
    <div ref={containerRef} className="hotkey-recorder-wrap">
      <button
        id={id}
        type="button"
        className={`hotkey-recorder ${recording ? 'hotkey-recorder--recording' : ''}`}
        onClick={() => !recording && startRecording()}
        onKeyDown={(e: ReactKeyboardEvent) => {
          if (!recording && (e.key === 'Enter' || e.key === ' ')) {
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
          ? `${ariaLabel}: ${recording ? recordingPlaceholder : (value || defaultPlaceholder)}`
          : a11y.labelledBy ? undefined : (value || defaultPlaceholder)}
      >
        <span id={`${id}-value`} className="sr-only">
          {recording ? recordingPlaceholder : (value || defaultPlaceholder)}
        </span>
        <span aria-hidden="true" style={{ display: 'inline-flex', alignItems: 'center' }}>
          {recording ? (
            activeKeys.length > 0 ? renderKeys(activeKeys) : (
              <span className="hotkey-recorder__placeholder">{recordingPlaceholder}</span>
            )
          ) : (
            parsedKeys.length > 0 ? renderKeys(parsedKeys) : (
              <span className="hotkey-recorder__placeholder">{defaultPlaceholder}</span>
            )
          )}
        </span>
      </button>
      {value && !recording && (
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

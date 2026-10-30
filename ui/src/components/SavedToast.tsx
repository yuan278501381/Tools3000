/* ─────────────────────────────────────────────────────────────────────────────
 * SavedToast.tsx — 世界级设置保存胶囊 Toast 提示组件
 *
 * 视觉: 居中偏下悬浮药丸胶囊 (Pill Capsule)，亚克力毛玻璃、深浅双态、微边框阴影。
 * 动效: 弹性入场、微脉冲防抖延期、错误微晃动、平滑退场。
 * 架构: 支持 IPC 智能拦截器自动广播与命令式调用，多态支持（成功/失败/警告）。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useRef, useCallback, type FC } from 'react';
import { CheckCircle2, AlertCircle, AlertTriangle, Info } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { type SavedToastType, type SavedToastDetail } from './savedToastModel';
import './SavedToast.css';

export const SavedToast: FC = () => {
  const { t } = useTranslation();
  const [visible, setVisible] = useState(false);
  const [exiting, setExiting] = useState(false);
  const [pulsing, setPulsing] = useState(false);
  const [toastData, setToastData] = useState<{ message: string; type: SavedToastType }>({
    message: '',
    type: 'success',
  });

  const visibleRef = useRef(false);
  const exitingRef = useRef(false);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const exitTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pulseTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const clearAllTimers = useCallback(() => {
    if (timerRef.current) {
      clearTimeout(timerRef.current);
      timerRef.current = null;
    }
    if (exitTimerRef.current) {
      clearTimeout(exitTimerRef.current);
      exitTimerRef.current = null;
    }
    if (pulseTimerRef.current) {
      clearTimeout(pulseTimerRef.current);
      pulseTimerRef.current = null;
    }
  }, []);

  const dismiss = useCallback(() => {
    if (!visibleRef.current || exitingRef.current) return;
    exitingRef.current = true;
    setExiting(true);
    if (exitTimerRef.current) clearTimeout(exitTimerRef.current);
    exitTimerRef.current = setTimeout(() => {
      visibleRef.current = false;
      exitingRef.current = false;
      setVisible(false);
      setExiting(false);
    }, 180);
  }, []);

  const triggerToast = useCallback((detail?: SavedToastDetail, defaultType: SavedToastType = 'success') => {
    const toastType = detail?.type || defaultType;
    let text = detail?.message?.trim() || '';
    if (!text) {
      text = toastType === 'error' ? t('common.saveFailed') : t('common.saved');
    }

    setToastData({ message: text, type: toastType });
    if (exitTimerRef.current) {
      clearTimeout(exitTimerRef.current);
      exitTimerRef.current = null;
    }
    exitingRef.current = false;
    setExiting(false);

    if (visibleRef.current) {
      // 当前已在展示，触发微脉冲动效并延展倒计时
      setPulsing(false);
      if (pulseTimerRef.current) clearTimeout(pulseTimerRef.current);
      requestAnimationFrame(() => {
        setPulsing(true);
        pulseTimerRef.current = setTimeout(() => setPulsing(false), 260);
      });
    } else {
      visibleRef.current = true;
      setVisible(true);
    }

    if (timerRef.current) clearTimeout(timerRef.current);
    const duration = detail?.duration || (toastType === 'error' ? 3500 : 1800);
    timerRef.current = setTimeout(() => {
      dismiss();
    }, duration);
  }, [dismiss, t]);

  useEffect(() => {
    const handleSaved = (e: Event) => {
      const detail = (e as CustomEvent<SavedToastDetail>).detail;
      triggerToast(detail, 'success');
    };

    const handleSaveFailed = (e: Event) => {
      const detail = (e as CustomEvent<SavedToastDetail>).detail;
      triggerToast(detail, 'error');
    };

    window.addEventListener('tools3000:settings-saved', handleSaved);
    window.addEventListener('tools3000:settings-save-failed', handleSaveFailed);

    return () => {
      window.removeEventListener('tools3000:settings-saved', handleSaved);
      window.removeEventListener('tools3000:settings-save-failed', handleSaveFailed);
      clearAllTimers();
    };
  }, [clearAllTimers, triggerToast]);

  if (!visible) return null;

  const renderIcon = () => {
    switch (toastData.type) {
      case 'error':
        return <AlertCircle size={16} strokeWidth={2.5} />;
      case 'warning':
        return <AlertTriangle size={16} strokeWidth={2.5} />;
      case 'info':
        return <Info size={16} strokeWidth={2.5} />;
      case 'success':
      default:
        return <CheckCircle2 size={16} strokeWidth={2.5} />;
    }
  };

  const classNames = [
    'saved-toast',
    `saved-toast--${toastData.type}`,
    pulsing ? 'saved-toast--pulse' : '',
    exiting ? 'saved-toast--exiting' : '',
  ].filter(Boolean).join(' ');

  return (
    <div className="saved-toast-host" role="status" aria-live="polite">
      <div className={classNames} onClick={dismiss} title={toastData.message}>
        <span className="saved-toast__icon">
          {renderIcon()}
        </span>
        <span className="saved-toast__text">{toastData.message}</span>
      </div>
    </div>
  );
};

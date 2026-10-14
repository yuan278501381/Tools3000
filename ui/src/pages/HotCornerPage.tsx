/* ─────────────────────────────────────────────────────────────────────────────
 * HotCornerPage — 屏幕触发角可视化配置页
 *
 * 功能:
 *   - 中央屏幕示意图 (圆角矩形 + 渐变背景)
 *   - 四角各有一个 Corner Card（显示当前动作 / 允许选择新动作）
 *   - 触发延迟滑块 (100ms ~ 1000ms)
 *   - 全局启用 / 禁用 Toggle
 *   - IPC: hotcorner.getSettings / hotcorner.updateSettings
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Select } from '../components/UIKit';
import { bridgeRequest } from '../hooks/useBridge';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { MonitorUp } from 'lucide-react';
import { BUILTIN_COMMAND_KEYS } from '../components/gestureModel';
import './HotCornerPage.css';

/* ── 类型定义 ─────────────────────────────────────────────────────────────── */

type CornerPosition = 'topLeft' | 'topRight' | 'bottomLeft' | 'bottomRight';

interface CornerAction {
  /** -1 = 未设置, 0..N = 内置命令索引 */
  commandIndex: number;
}

interface HotCornerSettings {
  enabled: boolean;
  autoBypassFullscreen?: boolean;
  delay: number;
  corners: Record<CornerPosition, CornerAction>;
}

interface OperationResult {
  success: boolean;
  error?: string;
}

const CORNER_POSITIONS: { key: CornerPosition; cssClass: string; labelKey: 'hotcorner.cornerTL' | 'hotcorner.cornerTR' | 'hotcorner.cornerBL' | 'hotcorner.cornerBR' }[] = [
  { key: 'topLeft',     cssClass: 'tl', labelKey: 'hotcorner.cornerTL' },
  { key: 'topRight',    cssClass: 'tr', labelKey: 'hotcorner.cornerTR' },
  { key: 'bottomLeft',  cssClass: 'bl', labelKey: 'hotcorner.cornerBL' },
  { key: 'bottomRight', cssClass: 'br', labelKey: 'hotcorner.cornerBR' },
];

const DEFAULT_SETTINGS: HotCornerSettings = {
  enabled: false,
  autoBypassFullscreen: true,
  delay: 300,
  corners: {
    topLeft:     { commandIndex: -1 },
    topRight:    { commandIndex: -1 },
    bottomLeft:  { commandIndex: -1 },
    bottomRight: { commandIndex: -1 },
  },
};

/* ── 组件 ─────────────────────────────────────────────────────────────────── */

export const HotCornerPage: FC = () => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<HotCornerSettings>(DEFAULT_SETTINGS);
  const [loading, setLoading] = useState(true);

  // ── 加载设置 ────────────────────────────────────────────────────────────
  const reload = useCallback(async () => {
    const data = await bridgeRequest<HotCornerSettings>('hotcorner.getSettings');
    setSettings(prev => ({ ...prev, ...data }));
  }, []);

  useEffect(() => {
    let active = true;
    bridgeRequest<HotCornerSettings>('hotcorner.getSettings')
      .then(data => { if (active) setSettings(prev => ({ ...prev, ...data })); })
      .catch(error => {
        console.error('Failed to load hot-corner settings:', error);
        if (active) toast.error(t('hotcorner.loadFailed'));
      })
      .finally(() => { if (active) setLoading(false); });
    return () => { active = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const persistPatch = useCallback(async (patch: Record<string, unknown>) => {
    try {
      const result = await bridgeRequest<OperationResult>('hotcorner.updateSettings', patch);
      if (!result.success) throw new Error(result.error || t('hotcorner.saveFailed'));
    } catch (error) {
      toast.error(t('hotcorner.saveFailed'), { description: String(error) });
      try { await reload(); } catch { /* keep the page usable */ }
    }
  }, [reload, t]);

  // ── 更新设置 ────────────────────────────────────────────────────────────
  const updateSetting = useCallback(<K extends keyof HotCornerSettings>(
    key: K, value: HotCornerSettings[K],
  ) => {
    setSettings(prev => ({ ...prev, [key]: value }));
    void persistPatch({ [key]: value });
  }, [persistPatch]);

  const updateCorner = useCallback((position: CornerPosition, commandIndex: number) => {
    setSettings(prev => {
      const next = {
        ...prev,
        corners: {
          ...prev.corners,
          [position]: { commandIndex },
        },
      };
      void persistPatch({ corners: next.corners });
      return next;
    });
  }, [persistPatch]);

  if (loading) {
    return <div style={{ padding: '2rem', opacity: 0.5 }}>{t('common.loading')}</div>;
  }

  // ── 构建动作选项列表 ───────────────────────────────────────────────────
  const actionOptions = [
    { value: '-1', label: t('hotcorner.noAction') },
    ...BUILTIN_COMMAND_KEYS.map((key, i) => ({ value: String(i), label: t(key) })),
  ];

  return (
    <div className="hotcorner-page">
      {/* ── 全局开关 ──────────────────────────────────────────────────── */}
      <SettingGroup title={t('hotcorner.title')} icon={<MonitorUp size={20} strokeWidth={2.5} />}>
        <Card>
          <Toggle
            id="hotcorner-enabled"
            label={t('hotcorner.enabled')}
            description={t('hotcorner.enabledDesc')}
            checked={settings.enabled}
            onChange={(v) => updateSetting('enabled', v)}
          />
          <Toggle
            id="hotcorner-auto-bypass"
            label={t('hotcorner.autoBypassFullscreen')}
            description={t('hotcorner.autoBypassFullscreenDesc')}
            checked={settings.autoBypassFullscreen ?? true}
            onChange={(v) => updateSetting('autoBypassFullscreen', v)}
          />
        </Card>
      </SettingGroup>

      {/* ── 屏幕示意图 ────────────────────────────────────────────────── */}
      <SettingGroup title={t('hotcorner.corners')} icon={<MonitorUp size={20} strokeWidth={2.5} />}>
        <Card>
          <div className="hotcorner-page__monitor-wrapper">
            <div className="hotcorner-page__monitor-assembly">
              <div className="hotcorner-page__monitor">
                {/* 屏幕中央标签 */}
                <div className="hotcorner-page__monitor-label">
                  <MonitorUp size={32} className="hotcorner-page__monitor-label-icon" />
                  <span className="hotcorner-page__monitor-label-text">
                    {t('hotcorner.screenHint')}
                  </span>
                </div>

                {/* 四角装饰连接线 */}
                <div className="hotcorner-page__connector hotcorner-page__connector--tl" />
                <div className="hotcorner-page__connector hotcorner-page__connector--tr" />
                <div className="hotcorner-page__connector hotcorner-page__connector--bl" />
                <div className="hotcorner-page__connector hotcorner-page__connector--br" />

                {/* 四角卡片 */}
                {CORNER_POSITIONS.map(({ key, cssClass, labelKey }) => {
                  const action = settings.corners[key];
                  const hasAction = action.commandIndex >= 0;
                  const isBottom = key === 'bottomLeft' || key === 'bottomRight';

                  return (
                    <div
                      key={key}
                      className={`hotcorner-page__corner hotcorner-page__corner--${cssClass}`}
                    >
                      <div className="hotcorner-page__corner-header">
                        <div className="hotcorner-page__corner-label">
                          {t(labelKey)}
                        </div>
                        <span
                          className={`hotcorner-page__corner-indicator ${!hasAction ? 'hotcorner-page__corner-indicator--empty' : ''}`}
                        />
                      </div>
                      <div className="hotcorner-page__corner-select">
                        <Select
                          id={`corner-${key}`}
                          value={String(action.commandIndex)}
                          options={actionOptions}
                          onChange={(v) => updateCorner(key, parseInt(v))}
                          placement={isBottom ? 'top' : 'bottom'}
                          portal
                        />
                      </div>
                    </div>
                  );
                })}
              </div>
              <div className="hotcorner-page__monitor-stand" />
              <div className="hotcorner-page__monitor-base" />
            </div>
          </div>
        </Card>
      </SettingGroup>

      {/* ── 触发延迟 ──────────────────────────────────────────────────── */}
      <SettingGroup title={t('hotcorner.advanced')} icon={<MonitorUp size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow
            label={t('hotcorner.delay')}
            description={t('hotcorner.delayDesc')}
          >
            <div className="hotcorner-page__slider-wrapper">
              <input
                type="range"
                className="hotcorner-page__slider"
                min={100}
                max={1000}
                step={50}
                value={settings.delay}
                onChange={(e) => setSettings(prev => ({ ...prev, delay: parseInt(e.target.value) }))}
                onPointerUp={() => void persistPatch({ delay: settings.delay })}
                onKeyUp={() => void persistPatch({ delay: settings.delay })}
              />
              <span className="hotcorner-page__slider-value">{settings.delay}ms</span>
            </div>
          </SettingRow>
        </Card>
      </SettingGroup>
    </div>
  );
};

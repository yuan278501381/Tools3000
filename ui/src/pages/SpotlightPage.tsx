/* ─────────────────────────────────────────────────────────────────────────────
 * SpotlightPage.tsx — 寻找鼠标与演示特效配置中心 (世界级 UX 双态色彩体系)
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { Card, Toggle, SettingGroup, Button, NumberInput, Select } from '../components/UIKit';
import { bridgeRequest, useBridgeEvent } from '../hooks/useBridge';
import { toast } from 'sonner';
import {
  MousePointerClick,
  RotateCcw,
  Play,
  Activity,
  Sparkles,
  Palette,
  Waves,
  Crosshair,
  Flame,
  CircleDot,
  Rainbow,
  Atom,
  Zap,
  PenTool,
  Compass,
  Droplets,
  Shield,
  Circle,
  ZoomIn,
} from 'lucide-react';
import './SpotlightPage.css';

interface SpotlightSettings {
  enabled: boolean;
  triggerDoubleCtrl: boolean;
  triggerShakeMouse: boolean;
  autoBypassFullscreen: boolean;
  spotlightColor: string;
  spotlightSize: number;
  animationDurationMs: number;
  holdDurationMs: number;
  shakeThreshold: number;
  spotlightAnimStyle: string;

  clickRippleEnabled: boolean;
  clickRippleStyle: string;
  mouseTrailEnabled: boolean;
  mouseTrailStyle: string;
  mouseTrailColorMode: string;
  leftClickColor: string;
  rightClickColor: string;
  middleClickColor: string;
}

const DEFAULT_SETTINGS: SpotlightSettings = {
  enabled: true,
  triggerDoubleCtrl: true,
  triggerShakeMouse: false,
  autoBypassFullscreen: true,
  spotlightColor: 'auto',
  spotlightSize: 300,
  animationDurationMs: 1000,
  holdDurationMs: 0,
  shakeThreshold: 4,
  spotlightAnimStyle: 'inward_gravity',

  clickRippleEnabled: false,
  clickRippleStyle: 'sparkle_burst',
  mouseTrailEnabled: false,
  mouseTrailStyle: 'sonar_pulses',
  mouseTrailColorMode: 'rainbow',
  leftClickColor: 'auto',
  rightClickColor: '#fb7185',
  middleClickColor: '#fbbf24',
};

const ACCENT_COLOR_MAP: Record<string, string> = {
  blue: '#3b82f6',
  cyan: '#06b6d4',
  amber: '#f59e0b',
  mint: '#10b981',
  coral: '#f43f5e',
  violet: '#8b5cf6',
};

/** 颜色双态分段胶囊组件 */
interface ColorSegmentControlProps {
  label: string;
  desc?: string;
  value: string;
  defaultCustomFallback: string;
  brandAccentHex: string;
  onChange: (val: string) => void;
}

const ColorSegmentControl: FC<ColorSegmentControlProps> = ({
  label,
  desc,
  value,
  defaultCustomFallback,
  brandAccentHex,
  onChange,
}) => {
  const { t } = useTranslation();
  const isAuto = value === 'auto';
  const displayHex = isAuto ? brandAccentHex : value;

  return (
    <div className="spotlight-page__prop-card spotlight-page__color-card">
      <div className="spotlight-page__prop-header">
        <span className="spotlight-page__prop-title">{label}</span>
        {desc && <span className="spotlight-page__prop-desc">{desc}</span>}
      </div>

      {/* 双态胶囊选择器 */}
      <div className="spotlight-page__capsule-wrap">
        <button
          type="button"
          className={`spotlight-page__capsule-btn ${isAuto ? 'active' : ''}`}
          onClick={() => onChange('auto')}
        >
          <Sparkles size={13} />
          <span>{t('spotlight.followBrandAccent', 'Follow Theme Accent')}</span>
          <span className="spotlight-page__capsule-dot" style={{ backgroundColor: brandAccentHex }} />
        </button>

        <button
          type="button"
          className={`spotlight-page__capsule-btn ${!isAuto ? 'active' : ''}`}
          onClick={() => {
            if (isAuto) {
              onChange(defaultCustomFallback);
            }
          }}
        >
          <Palette size={13} />
          <span>{t('spotlight.customColor', 'Custom Color')}</span>
        </button>
      </div>

      {/* 色彩展示与调色盘操作 */}
      <div className="spotlight-page__prop-body">
        <div className="spotlight-page__color-row">
          <div className="spotlight-page__color-info">
            <span className="spotlight-page__color-hex">{displayHex.toUpperCase()}</span>
            {!isAuto && (
              <button
                type="button"
                className="spotlight-page__restore-capsule"
                onClick={() => onChange('auto')}
                title={t('spotlight.restoreFollowBrandDesc', 'Switch back and link dynamically to Tools3000 theme accent color')}
              >
                <RotateCcw size={11} />
                <span>{t('spotlight.restoreFollowBrand', 'Follow Theme Accent')}</span>
              </button>
            )}
          </div>

          <div className="spotlight-page__color-picker-wrap">
            <div
              className="spotlight-page__color-swatch"
              style={{ backgroundColor: displayHex }}
              title={isAuto ? t('spotlight.followBrandAccent') : displayHex}
            />
            {!isAuto && (
              <input
                type="color"
                className="spotlight-page__color-input"
                value={displayHex}
                onChange={(e) => onChange(e.target.value)}
                aria-label={label}
              />
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

export const SpotlightPage: FC = () => {
  const { t } = useTranslation();
  const [settings, setSettings] = useState<SpotlightSettings>(DEFAULT_SETTINGS);
  const [loading, setLoading] = useState(true);
  const [accent, setAccent] = useState<string>(() => {
    try {
      return localStorage.getItem('tools3000:accent-color') || 'blue';
    } catch {
      return 'blue';
    }
  });

  useEffect(() => {
    const handleAccent = (e: Event) => {
      const detail = (e as CustomEvent<string>).detail;
      if (detail) setAccent(detail);
    };
    window.addEventListener('tools3000:accent-changed', handleAccent);
    return () => window.removeEventListener('tools3000:accent-changed', handleAccent);
  }, []);

  useEffect(() => {
    bridgeRequest<SpotlightSettings>('spotlight.getSettings')
      .then((res) => {
        if (res) {
          setSettings((prev) => ({ ...prev, ...res }));
        }
      })
      .catch((err) => {
        console.error('Failed to load spotlight settings:', err);
        void Promise.allSettled([
          bridgeRequest<Array<{ id: string; enabled: boolean }>>('plugins.getAll'),
          bridgeRequest<boolean>('config.get', { key: '/plugins/spotlight/enabled' }),
          bridgeRequest<boolean>('config.get', { key: '/spotlight/enabled' }),
        ]).then(([pluginsRes, pluginCfgRes, legacyRes]) => {
          let effectiveEnabled = true;
          if (pluginsRes.status === 'fulfilled' && Array.isArray(pluginsRes.value)) {
            const item = pluginsRes.value.find((p) => p.id === 'spotlight');
            if (item && typeof item.enabled === 'boolean') effectiveEnabled = item.enabled;
          } else if (pluginCfgRes.status === 'fulfilled' && typeof pluginCfgRes.value === 'boolean') {
            effectiveEnabled = pluginCfgRes.value;
          } else if (legacyRes.status === 'fulfilled' && typeof legacyRes.value === 'boolean') {
            effectiveEnabled = legacyRes.value;
          }
          setSettings((prev) => ({ ...prev, enabled: effectiveEnabled }));
        });
      })
      .finally(() => setLoading(false));
  }, []);

  useBridgeEvent('spotlight:stateChanged', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { enabled?: boolean }) : null;
    if (payload && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setSettings((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  useBridgeEvent('plugins:changed', (data: unknown) => {
    const payload = data && typeof data === 'object' ? (data as { id?: string; enabled?: boolean }) : null;
    if (payload && payload.id === 'spotlight' && typeof payload.enabled === 'boolean') {
      const nextEnabled = payload.enabled;
      setSettings((prev) => ({ ...prev, enabled: nextEnabled }));
    }
  });

  const saveSetting = async <K extends keyof SpotlightSettings>(key: K, value: SpotlightSettings[K]) => {
    const previous = settings[key];
    const updated = { ...settings, [key]: value };
    setSettings(updated);
    try {
      if (key === 'enabled') {
        await Promise.all([
          bridgeRequest('plugins.setEnabled', { id: 'spotlight', enabled: Boolean(value) }).catch(() => {}),
          bridgeRequest('spotlight.updateSettings', { [key]: value }).catch(() => {}),
        ]);
      } else {
        await bridgeRequest('spotlight.updateSettings', { [key]: value });
      }
    } catch (e) {
      setSettings((prev) => ({ ...prev, [key]: previous }));
      toast.error(t('spotlight.saveFailed', 'Failed to save spotlight settings'), { description: String(e) });
    }
  };

  const handleResetDefaults = async () => {
    try {
      await bridgeRequest('spotlight.resetDefaults', {});
      setSettings(DEFAULT_SETTINGS);
      toast.success(t('spotlight.resetSuccess', 'Restored default spotlight settings'));
    } catch (e) {
      toast.error(t('spotlight.saveFailed', 'Failed to save spotlight settings'), { description: String(e) });
    }
  };

  const handleTestSpotlight = async () => {
    try {
      await bridgeRequest('spotlight.trigger', {});
    } catch (e) {
      console.error('Failed to trigger spotlight:', e);
    }
  };

  const currentBrandAccentHex = ACCENT_COLOR_MAP[accent] || '#3b82f6';

  if (loading) {
    return <div style={{ padding: '2rem', opacity: 0.5 }}>{t('common.loading', 'Loading...')}</div>;
  }

  return (
    <div className="spotlight-page">
      {/* ── 顶部操作栏 ──────────────────────────────────────────────── */}
      <div className="spotlight-page__header">
        <div className="spotlight-page__title-wrap">
          <h2 className="spotlight-page__title">{t('spotlight.title', 'Mouse Presentation & FX')}</h2>
        </div>
        <Button variant="ghost" onClick={handleResetDefaults} title={t('spotlight.resetDefaults', 'Reset to Defaults')}>
          <RotateCcw size={14} style={{ marginRight: 6 }} />
          <span>{t('spotlight.resetDefaults', 'Reset to Defaults')}</span>
        </Button>
      </div>

      {/* ── 1. 主开关与全屏免打扰 ────────────────────────────────────── */}
      <Card>
        <Toggle
          id="spotlight-main-enabled"
          label={t('spotlight.mainToggle', 'Enable Mouse Presentation & FX')}
          description={t('spotlight.mainToggleDesc', 'Enable spotlight cursor focus, click ripples, and motion trail effects.')}
          checked={settings.enabled}
          onChange={(v) => saveSetting('enabled', v)}
        />
        <Toggle
          id="spotlight-auto-bypass"
          label={t('spotlight.autoBypassFullscreen', 'Fullscreen Game/Video Bypass')}
          description={t('spotlight.autoBypassFullscreenDesc', 'Automatically suppress spotlight when foreground window is in exclusive fullscreen mode to prevent disrupting gaming or video playback')}
          checked={settings.autoBypassFullscreen}
          onChange={(v) => saveSetting('autoBypassFullscreen', v)}
        />
      </Card>

      {/* ── 2. 触发方式 ────────────────────────────────────────────── */}
      <SettingGroup title={t('spotlight.triggerSection', 'Trigger Methods')} icon={<Activity size={18} />}>
        <Card>
          <Toggle
            id="spotlight-double-ctrl"
            label={t('spotlight.doubleCtrl', 'Double-press Ctrl')}
            description={t('spotlight.doubleCtrlDesc', 'Trigger by pressing Control key twice.')}
            checked={settings.triggerDoubleCtrl}
            onChange={(v) => saveSetting('triggerDoubleCtrl', v)}
          />
          <Toggle
            id="spotlight-shake-mouse"
            label={t('spotlight.shakeMouse', 'Shake Mouse')}
            description={t('spotlight.shakeMouseDesc', 'Trigger by shaking cursor quickly.')}
            checked={settings.triggerShakeMouse}
            onChange={(v) => saveSetting('triggerShakeMouse', v)}
          />
        </Card>
      </SettingGroup>

      {/* ── 3. 外观样式 ────────────────────────────────────────────── */}
      <SettingGroup title={t('spotlight.appearanceSection', 'Appearance & Style')} icon={<Sparkles size={18} />}>
        {/* 聚光灯全屏聚焦动效选择器 */}
        <Card>
          <div className="spotlight-page__section-label" style={{ marginBottom: '12px' }}>
            <span className="spotlight-page__section-title">
              <Sparkles size={15} />
              <span>{t('spotlight.animStyle', 'Full-Screen Focus Animation')}</span>
            </span>
            <span className="spotlight-page__section-desc">{t('spotlight.animStyleDesc', 'Full-screen visual transition and inward visual guiding animation on spotlight trigger')}</span>
          </div>
          <div className="spotlight-page__style-grid">
            {/* 1. 向心引力折叠 (默认推荐) */}
            <div
              className={`spotlight-page__style-card ${settings.spotlightAnimStyle === 'inward_gravity' ? 'spotlight-page__style-card--active' : ''}`}
              onClick={async () => {
                await saveSetting('spotlightAnimStyle', 'inward_gravity');
                void bridgeRequest('spotlight.trigger').catch(() => {});
              }}
            >
              <div className="spotlight-page__style-card-header">
                <Sparkles size={16} />
                <span>{t('spotlight.styleInwardGravity', 'Inward Gravity (Recommended)')}</span>
              </div>
              <span className="spotlight-page__style-card-desc">{t('spotlight.styleInwardGravityDesc', 'Dual high-speed inward rings collapse to mouse center with spring pulse & cinematic vignette')}</span>
            </div>

            {/* 2. 科技声纳雷达 */}
            <div
              className={`spotlight-page__style-card ${settings.spotlightAnimStyle === 'tactical_sonar' ? 'spotlight-page__style-card--active' : ''}`}
              onClick={async () => {
                await saveSetting('spotlightAnimStyle', 'tactical_sonar');
                void bridgeRequest('spotlight.trigger').catch(() => {});
              }}
            >
              <div className="spotlight-page__style-card-header">
                <Crosshair size={16} />
                <span>{t('spotlight.styleTacticalSonar', 'Tactical Sonar Radar')}</span>
              </div>
              <span className="spotlight-page__style-card-desc">{t('spotlight.styleTacticalSonarDesc', '3 outward sonar shockwaves with 4 rotating CAD tactical HUD arc reticles, geek tech style')}</span>
            </div>

            {/* 3. 极简极光涟漪 */}
            <div
              className={`spotlight-page__style-card ${settings.spotlightAnimStyle === 'aurora_ripple' ? 'spotlight-page__style-card--active' : ''}`}
              onClick={async () => {
                await saveSetting('spotlightAnimStyle', 'aurora_ripple');
                void bridgeRequest('spotlight.trigger').catch(() => {});
              }}
            >
              <div className="spotlight-page__style-card-header">
                <Waves size={16} />
                <span>{t('spotlight.styleAuroraRipple', 'Minimalist Aurora Ripple')}</span>
              </div>
              <span className="spotlight-page__style-card-desc">{t('spotlight.styleAuroraRippleDesc', 'Smooth Bezier dark vignette unfolding with gentle outer aurora ripples, soft and comfortable')}</span>
            </div>
          </div>
        </Card>

        <div className="spotlight-page__grid">
          {/* 聚光灯发光颜色 (双态胶囊) */}
          <ColorSegmentControl
            label={t('spotlight.spotlightColor', 'Spotlight Color')}
            desc={t('spotlight.spotlightColorDesc', 'Outer glow color.')}
            value={settings.spotlightColor}
            defaultCustomFallback="#3b82f6"
            brandAccentHex={currentBrandAccentHex}
            onChange={(val) => saveSetting('spotlightColor', val)}
          />

          {/* 聚光灯大小 */}
          <div className="spotlight-page__prop-card">
            <div className="spotlight-page__prop-header">
              <span className="spotlight-page__prop-title">{t('spotlight.spotlightSize', 'Spotlight Size')}</span>
              <span className="spotlight-page__prop-desc">{t('spotlight.spotlightSizeDesc', 'Circle diameter (pixels).')}</span>
            </div>
            <div className="spotlight-page__prop-body">
              <NumberInput
                min={80}
                max={600}
                step={20}
                unit="px"
                value={settings.spotlightSize}
                onChange={(v) => saveSetting('spotlightSize', v)}
                ariaLabel={t('spotlight.spotlightSize', 'Spotlight Size')}
              />
            </div>
          </div>

          {/* 动画时长 */}
          <div className="spotlight-page__prop-card">
            <div className="spotlight-page__prop-header">
              <span className="spotlight-page__prop-title">{t('spotlight.animDuration', 'Animation Duration')}</span>
              <span className="spotlight-page__prop-desc">{t('spotlight.animDurationDesc', 'Fade in/out speed (ms).')}</span>
            </div>
            <div className="spotlight-page__prop-body">
              <NumberInput
                min={200}
                max={3000}
                step={100}
                unit="ms"
                value={settings.animationDurationMs}
                onChange={(v) => saveSetting('animationDurationMs', v)}
                ariaLabel={t('spotlight.animDuration', 'Animation Duration')}
              />
            </div>
          </div>

          {/* 停留时长 */}
          <div className="spotlight-page__prop-card">
            <div className="spotlight-page__prop-header">
              <span className="spotlight-page__prop-title">{t('spotlight.holdDuration', 'Hold Duration')}</span>
              <span className="spotlight-page__prop-desc">{t('spotlight.holdDurationDesc', 'Stays visible continuously by default, or fades out after a set duration.')}</span>
            </div>
            <div className="spotlight-page__prop-body">
              <Select
                id="spotlight-hold-duration"
                value={String(settings.holdDurationMs <= 0 ? 0 : settings.holdDurationMs)}
                onChange={(v) => saveSetting('holdDurationMs', Math.max(0, Number(v) || 0))}
                options={[
                  { value: '0', label: t('spotlight.holdDurationNever', 'Never Auto-Exit (Default)') },
                  { value: '800', label: t('spotlight.holdDurationShort', '800 ms') },
                  { value: '1500', label: t('spotlight.holdDurationMedium', '1500 ms') },
                  { value: '3000', label: t('spotlight.holdDurationStandard', '3000 ms') },
                  { value: '5000', label: t('spotlight.holdDurationLong', '5000 ms') },
                  ...(![0, 800, 1500, 3000, 5000].includes(settings.holdDurationMs <= 0 ? 0 : settings.holdDurationMs)
                    ? [{
                        value: String(settings.holdDurationMs),
                        label: t('spotlight.holdDurationCustom', '{{ms}} ms (Custom)', { ms: settings.holdDurationMs }),
                      }]
                    : []),
                ]}
              />
            </div>
          </div>

          {/* 摇晃阈值 */}
          <div className="spotlight-page__prop-card">
            <div className="spotlight-page__prop-header">
              <span className="spotlight-page__prop-title">{t('spotlight.shakeThreshold', 'Shake Threshold')}</span>
              <span className="spotlight-page__prop-desc">{t('spotlight.shakeThresholdDesc', 'Shake detection sensitivity (default 7).')}</span>
            </div>
            <div className="spotlight-page__prop-body">
              <NumberInput
                min={3}
                max={20}
                step={1}
                value={settings.shakeThreshold}
                onChange={(v) => saveSetting('shakeThreshold', v)}
                ariaLabel={t('spotlight.shakeThreshold', 'Shake Threshold')}
              />
            </div>
          </div>
        </div>
      </SettingGroup>

      {/* ── 4. 鼠标点击与轨迹特效 (演示辅助 - 独立功能卡片与内嵌从属面板) ── */}
      <SettingGroup title={t('spotlight.mouseFxSection', 'Click & Motion Effects')} icon={<MousePointerClick size={18} />}>
        {/* 4.1 鼠标点击特效独立卡片 */}
        <Card>
          <Toggle
            id="spotlight-click-ripple"
            label={t('spotlight.clickRipple', 'Show Click Ripples')}
            description={t('spotlight.clickRippleDesc', 'Display temporary ripple circles at mouse click positions.')}
            checked={settings.clickRippleEnabled}
            onChange={(v) => saveSetting('clickRippleEnabled', v)}
          />

          {settings.clickRippleEnabled && (
            <div className="spotlight-page__inset-panel">
              {/* 点击动效风格矩阵 */}
              <div>
                <div className="spotlight-page__section-label" style={{ marginBottom: '10px' }}>
                  <span className="spotlight-page__section-title">
                    <Sparkles size={15} />
                    <span>{t('spotlight.clickStyle', 'Click Effect Style')}</span>
                  </span>
                  <span className="spotlight-page__section-desc">{t('spotlight.clickStyleDesc', 'Choose visual ripple and feedback style for mouse clicks')}</span>
                </div>
                <div className="spotlight-page__style-grid">
                  {/* 1. 星芒微粒迸发 (默认) */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'sparkle_burst' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'sparkle_burst')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Sparkles size={16} />
                      <span>{t('spotlight.styleSparkleBurst', 'Sparkle Burst')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleSparkleBurstDesc', 'Bursts of tiny glowing stardust sparks floating outward on click, lively and dynamic')}</span>
                  </div>

                  {/* 2. 流体光圈冲击波 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'ripple_ring' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'ripple_ring')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Waves size={16} />
                      <span>{t('spotlight.styleRippleRing', 'Fluid Ripple Shockwave')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleRippleRingDesc', 'Dual-layer fluid shockwave rings smoothly expanding outward, classic and intuitive')}</span>
                  </div>

                  {/* 3. 精密雷达靶心 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'target_pulse' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'target_pulse')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Crosshair size={16} />
                      <span>{t('spotlight.styleTargetPulse', 'Precision Target Pulse')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleTargetPulseDesc', 'Ultra-thin precision crosshair with converging focus ring, ideal for detailed code/UI demo')}</span>
                  </div>

                  {/* 4. 柔光微晕气泡 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'soft_glow' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'soft_glow')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <CircleDot size={16} />
                      <span>{t('spotlight.styleSoftGlow', 'Ambient Soft Glow')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleSoftGlowDesc', 'Minimalist soft radial vignette glow, calm, gentle and distraction-free')}</span>
                  </div>

                  {/* 5. 超新星微爆发 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'supernova' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'supernova')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Flame size={16} />
                      <span>{t('spotlight.styleSupernova', 'Supernova Microburst')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleSupernovaDesc', 'Core collapse with outward vibrant photon shockwave rings, deep and cosmic')}</span>
                  </div>

                  {/* 6. 电磁脉冲放电 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'emp_discharge' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'emp_discharge')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Zap size={16} />
                      <span>{t('spotlight.styleEmpDischarge', 'EMP Arc Discharge')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleEmpDischargeDesc', 'Branching electrical sparks discharging outward from click position, energetic and intense')}</span>
                  </div>

                  {/* 7. 宣纸墨滴晕染 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'ink_droplet' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'ink_droplet')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Droplets size={16} />
                      <span>{t('spotlight.styleInkDroplet', 'Zen Ink Dispersion')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleInkDropletDesc', 'Translucent Chinese calligraphy ink expanding softly and evaporating, poetic and graceful')}</span>
                  </div>

                  {/* 8. 六边形蜂巢锁定 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'hexagon_lock' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'hexagon_lock')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Shield size={16} />
                      <span>{t('spotlight.styleHexagonLock', 'Hexagon Grid Lock')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleHexagonLockDesc', 'Ultra-thin precision hexagonal reticle rotating and locking inward, professional and high-tech')}</span>
                  </div>

                  {/* 9. 微气泡轻破 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'bubble_pop' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'bubble_pop')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Circle size={16} />
                      <span>{t('spotlight.styleBubblePop', 'Micro Bubble Pop')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleBubblePopDesc', 'Crystal clear bubble expanding and popping into gentle mist droplets, airy and delightful')}</span>
                  </div>

                  {/* 10. macOS 弹性光圈 */}
                  <div
                    className={`spotlight-page__style-card ${settings.clickRippleStyle === 'mac_zoom' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('clickRippleStyle', 'mac_zoom')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <ZoomIn size={16} />
                      <span>{t('spotlight.styleMacZoom', 'macOS Elastic Aura')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleMacZoomDesc', 'Screen Studio style spring-damped press, elastic burst, and soft glow aura')}</span>
                  </div>
                </div>
              </div>

              {/* 独立按键点击颜色 */}
              <div style={{ paddingTop: '12px', borderTop: '1px solid var(--input-border)' }}>
                <div className="spotlight-page__section-label" style={{ marginBottom: '10px' }}>
                  <span className="spotlight-page__section-title">
                    <Palette size={15} />
                    <span>{t('spotlight.clickColorsSection')}</span>
                  </span>
                  <span className="spotlight-page__section-desc">{t('spotlight.clickColorsSectionDesc')}</span>
                </div>
                <div className="spotlight-page__grid">
                  {/* 左键颜色 */}
                  <ColorSegmentControl
                    label={t('spotlight.leftClickColor', 'Left Click Color')}
                    value={settings.leftClickColor}
                    defaultCustomFallback="#3b82f6"
                    brandAccentHex={currentBrandAccentHex}
                    onChange={(val) => saveSetting('leftClickColor', val)}
                  />

                  {/* 右键颜色 */}
                  <ColorSegmentControl
                    label={t('spotlight.rightClickColor', 'Right Click Color')}
                    value={settings.rightClickColor}
                    defaultCustomFallback="#fb7185"
                    brandAccentHex={currentBrandAccentHex}
                    onChange={(val) => saveSetting('rightClickColor', val)}
                  />

                  {/* 中键颜色 */}
                  <ColorSegmentControl
                    label={t('spotlight.middleClickColor', 'Middle Click Color')}
                    value={settings.middleClickColor}
                    defaultCustomFallback="#fbbf24"
                    brandAccentHex={currentBrandAccentHex}
                    onChange={(val) => saveSetting('middleClickColor', val)}
                  />
                </div>
              </div>
            </div>
          )}
        </Card>

        {/* 4.2 鼠标移动轨迹独立卡片 */}
        <Card>
          <Toggle
            id="spotlight-mouse-trail"
            label={t('spotlight.mouseTrail', 'Show Mouse Trail')}
            description={t('spotlight.mouseTrailDesc', 'Display a fading colored trail along cursor movement.')}
            checked={settings.mouseTrailEnabled}
            onChange={(v) => saveSetting('mouseTrailEnabled', v)}
          />

          {settings.mouseTrailEnabled && (
            <div className="spotlight-page__inset-panel">
              {/* 轨迹风格矩阵 */}
              <div>
                <div className="spotlight-page__section-label" style={{ marginBottom: '10px' }}>
                  <span className="spotlight-page__section-title">
                    <Waves size={15} />
                    <span>{t('spotlight.trailStyle', 'Trail Effect Style')}</span>
                  </span>
                  <span className="spotlight-page__section-desc">{t('spotlight.trailStyleDesc', 'Choose the mouse motion trail animation style that fits your workflow')}</span>
                </div>
                <div className="spotlight-page__style-grid">
                  {/* 1. 彩色声纳微环 (默认) */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'sonar_pulses' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'sonar_pulses')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <CircleDot size={16} />
                      <span>{t('spotlight.styleSonarPulses', 'Sonar Pulse Rings')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleSonarPulsesDesc', 'Sparse expanding colorful pulse footsteps, 95% screen transparency')}</span>
                  </div>

                  {/* 2. 七彩星尘光球 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'stardust_orbs' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'stardust_orbs')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Sparkles size={16} />
                      <span>{t('spotlight.styleStardustOrbs', 'Prism Stardust Orbs')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleStardustOrbsDesc', 'Sparse floating colorful orbs of various sizes, zero text distraction, preserves focus flow')}</span>
                  </div>

                  {/* 3. 量子引力微子 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'quantum_lens' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'quantum_lens')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Atom size={16} />
                      <span>{t('spotlight.styleQuantumLens', 'Quantum Graviton Orbits')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleQuantumLensDesc', 'Spinning quantum photons orbiting the cursor motion axis, futuristic and subtle')}</span>
                  </div>

                  {/* 4. 特斯拉电弧微流 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'tesla_arc' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'tesla_arc')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Zap size={16} />
                      <span>{t('spotlight.styleTeslaArc', 'Tesla Lightning Arc')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleTeslaArcDesc', 'Electric plasma sparks hopping between movement inflection nodes, dynamic and sharp')}</span>
                  </div>

                  {/* 5. 宣纸水墨烟云 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'zen_ink' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'zen_ink')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <PenTool size={16} />
                      <span>{t('spotlight.styleZenInk', 'Zen Ink Stream')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleZenInkDesc', 'Velocity-sensitive calligraphy ink stroke smoke trail, organic and relaxing')}</span>
                  </div>

                  {/* 6. CAD 矢量标尺 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'blueprint_grid' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'blueprint_grid')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Compass size={16} />
                      <span>{t('spotlight.styleBlueprintGrid', 'CAD Blueprint Ruler')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleBlueprintGridDesc', 'Ultra-fine coordinate axis reticles and 90-degree corner ticks, engineering-grade clarity')}</span>
                  </div>

                  {/* 7. 晨露微气泡 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'morning_dew' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'morning_dew')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Droplets size={16} />
                      <span>{t('spotlight.styleMorningDew', 'Morning Dew Bubbles')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleMorningDewDesc', 'Translucent floating micro bubbles gently ascending and popping, zero fatigue')}</span>
                  </div>

                  {/* 8. 极光流体丝带 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'aurora_ribbon' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'aurora_ribbon')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Waves size={16} />
                      <span>{t('spotlight.styleAuroraRibbon', 'Aurora Ribbon Stream')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleAuroraRibbonDesc', 'Ultra-thin translucent ribbon stream with vibrant gradients')}</span>
                  </div>

                  {/* 9. 经典彗星流光 */}
                  <div
                    className={`spotlight-page__style-card ${settings.mouseTrailStyle === 'classic_comet' ? 'spotlight-page__style-card--active' : ''}`}
                    onClick={() => saveSetting('mouseTrailStyle', 'classic_comet')}
                  >
                    <div className="spotlight-page__style-card-header">
                      <Flame size={16} />
                      <span>{t('spotlight.styleClassicComet', 'Classic Comet')}</span>
                    </div>
                    <span className="spotlight-page__style-card-desc">{t('spotlight.styleClassicCometDesc', 'Continuous glowing comet tail')}</span>
                  </div>
                </div>
              </div>

              {/* 轨迹色彩模式 */}
              <div style={{ paddingTop: '12px', borderTop: '1px solid var(--input-border)' }}>
                <div className="spotlight-page__prop-card" style={{ background: 'var(--card-bg)' }}>
                  <div className="spotlight-page__prop-header">
                    <span className="spotlight-page__prop-title">{t('spotlight.trailColorMode', 'Trail Color Mode')}</span>
                    <span className="spotlight-page__prop-desc">{t('spotlight.trailColorModeDesc', 'Configure trail color animation scheme')}</span>
                  </div>
                  <div className="spotlight-page__capsule-wrap" style={{ maxWidth: '380px' }}>
                    <button
                      type="button"
                      className={`spotlight-page__capsule-btn ${settings.mouseTrailColorMode === 'rainbow' ? 'spotlight-page__capsule-btn--active active' : ''}`}
                      onClick={() => saveSetting('mouseTrailColorMode', 'rainbow')}
                    >
                      <Rainbow size={14} />
                      <span>{t('spotlight.trailColorModeRainbow', 'Rainbow Spectrum Flow')}</span>
                    </button>
                    <button
                      type="button"
                      className={`spotlight-page__capsule-btn ${settings.mouseTrailColorMode === 'accent' ? 'spotlight-page__capsule-btn--active active' : ''}`}
                      onClick={() => saveSetting('mouseTrailColorMode', 'accent')}
                    >
                      <Palette size={14} />
                      <span>{t('spotlight.trailColorModeAccent', 'Follow Accent / Custom')}</span>
                    </button>
                  </div>
                </div>
              </div>
            </div>
          )}
        </Card>
      </SettingGroup>

      {/* ── 底部即刻体验栏 ──────────────────────────────────────────── */}
      <div className="spotlight-page__action-footer">
        <Button variant="primary" onClick={handleTestSpotlight}>
          <Play size={14} style={{ marginRight: 6 }} />
          <span>{t('spotlight.testSpotlight', 'Try Spotlight Now')}</span>
        </Button>
      </div>
    </div>
  );
};

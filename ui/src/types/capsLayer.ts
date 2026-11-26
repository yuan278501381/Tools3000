/* ─────────────────────────────────────────────────────────────────────────────
 * capsLayer.ts — Caps 引导改键模块前后端 IPC 契约单一事实源 (SSOT)
 * ───────────────────────────────────────────────────────────────────────────── */

export type CapsTapAction = 'toggle_caps' | 'esc' | 'none';
export type CapsTargetType = 'key' | 'combo' | 'text';
export type CapsPresetId = 'arrows_ijkl' | 'arrows_vim' | 'arrows_esdf';

export interface CapsKeyMapping {
  id: string;
  key: string;
  targetType: CapsTargetType;
  targetValue: string;
  label?: string;
  enabled: boolean;
}

export interface CapsAppProfile {
  id: string;
  name: string;
  processName: string;
  enabled: boolean;
  mappings: CapsKeyMapping[];
}

export interface CapsLayerSettings {
  enabled: boolean;
  tapAction: CapsTapAction;
  shiftCapsTogglesNative: boolean;
  excludedApps: string[];
  globalMappings: CapsKeyMapping[];
  appProfiles: CapsAppProfile[];
}

export const DEFAULT_CAPS_LAYER_SETTINGS: CapsLayerSettings = {
  enabled: false,
  tapAction: 'toggle_caps',
  shiftCapsTogglesNative: true,
  excludedApps: [],
  globalMappings: [],
  appProfiles: [],
};

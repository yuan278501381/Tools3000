export interface StudioBackdropOption {
  readonly value: string;
  readonly labelKey: string;
}

const LEGACY_STUDIO_THEME_TO_CANONICAL: Record<string, string> = {
  aurora_purple: 'studio_slate',
  sunrise_coral: 'paper_chalk',
  emerald_sea: 'silk_mist',
  obsidian_dark: 'midnight_graphite',
  minimal_light: 'pure_minimal',
};

export function normalizeStudioBackdropKey(raw?: string): string {
  if (!raw) return 'blossom_silk';
  const low = raw.trim().toLowerCase();
  return LEGACY_STUDIO_THEME_TO_CANONICAL[low] ?? low;
}

export const STUDIO_BACKDROP_OPTIONS = [
  // 大师策展首屏旗舰原创壁纸 (21)
  { value: 'blossom_silk', labelKey: 'capture.themeBlossomSilk' },
  { value: 'crimson_velvet', labelKey: 'capture.themeCrimsonVelvet' },
  { value: 'obsidian_gold', labelKey: 'capture.themeObsidianGold' },
  { value: 'abyssal_indigo', labelKey: 'capture.themeAbyssalIndigo' },
  { value: 'lavender_mist', labelKey: 'capture.themeLavenderMist' },
  { value: 'sunlit_cream', labelKey: 'capture.themeSunlitCream' },
  { value: 'desert_dunes', labelKey: 'capture.themeDesertDunes' },
  { value: 'mint_porcelain', labelKey: 'capture.themeMintPorcelain' },
  { value: 'coastal_horizon', labelKey: 'capture.themeCoastalHorizon' },
  { value: 'carbon_aurora', labelKey: 'capture.themeCarbonAurora' },
  { value: 'aurora_night', labelKey: 'capture.themeAuroraNight' },
  { value: 'alpine_sunrise', labelKey: 'capture.themeAlpineSunrise' },
  { value: 'dusk_mountains', labelKey: 'capture.themeDuskMountains' },
  { value: 'misty_lake_dawn', labelKey: 'capture.themeMistyLakeDawn' },
  { value: 'nordic_fjord', labelKey: 'capture.themeNordicFjord' },
  { value: 'studio_slate', labelKey: 'capture.themeAuroraPurple' },
  { value: 'silk_mist', labelKey: 'capture.themeEmeraldSea' },
  { value: 'midnight_graphite', labelKey: 'capture.themeObsidianDark' },
  { value: 'pure_minimal', labelKey: 'capture.themeMinimalLight' },
  { value: 'paper_chalk', labelKey: 'capture.themeSunriseCoral' },
  { value: 'airy_glacier', labelKey: 'capture.themeAiryGlacier' },
  // 流体丝绸极光着色器 (21)
  { value: 'mesh_glacier_sky', labelKey: 'capture.themeMeshGlacierSky' },
  { value: 'mesh_azure_wave', labelKey: 'capture.themeMeshAzureWave' },
  { value: 'mesh_indigo_pulse', labelKey: 'capture.themeMeshIndigoPulse' },
  { value: 'mesh_aurora_mint', labelKey: 'capture.themeMeshAuroraMint' },
  { value: 'mesh_breeze_aqua', labelKey: 'capture.themeMeshBreezeAqua' },
  { value: 'mesh_nordic_blue', labelKey: 'capture.themeMeshNordicBlue' },
  { value: 'mesh_cobalt_bloom', labelKey: 'capture.themeMeshCobaltBloom' },
  { value: 'mesh_peach_blossom', labelKey: 'capture.themeMeshPeachBlossom' },
  { value: 'mesh_sunset_glow', labelKey: 'capture.themeMeshSunsetGlow' },
  { value: 'mesh_champagne_gold', labelKey: 'capture.themeMeshChampagneGold' },
  { value: 'mesh_rose_quartz', labelKey: 'capture.themeMeshRoseQuartz' },
  { value: 'mesh_amber_sand', labelKey: 'capture.themeMeshAmberSand' },
  { value: 'mesh_lilac_cloud', labelKey: 'capture.themeMeshLilacCloud' },
  { value: 'mesh_velvet_berry', labelKey: 'capture.themeMeshVelvetBerry' },
  { value: 'mesh_dark_obsidian', labelKey: 'capture.themeMeshDarkObsidian' },
  { value: 'mesh_dark_violet', labelKey: 'capture.themeMeshDarkViolet' },
  { value: 'mesh_dark_emerald', labelKey: 'capture.themeMeshDarkEmerald' },
  { value: 'mesh_dark_titanium', labelKey: 'capture.themeMeshDarkTitanium' },
  { value: 'mesh_dark_amber', labelKey: 'capture.themeMeshDarkAmber' },
  { value: 'mesh_dark_crimson', labelKey: 'capture.themeMeshDarkCrimson' },
  { value: 'mesh_silver_mist', labelKey: 'capture.themeMeshSilverMist' },
  // 极简高定纯色 (7)
  { value: 'solid_snow_white', labelKey: 'capture.themeSolidSnowWhite' },
  { value: 'solid_warm_linen', labelKey: 'capture.themeSolidWarmLinen' },
  { value: 'solid_cool_slate', labelKey: 'capture.themeSolidCoolSlate' },
  { value: 'solid_apple_blue', labelKey: 'capture.themeSolidAppleBlue' },
  { value: 'solid_sage_gray', labelKey: 'capture.themeSolidSageGray' },
  { value: 'solid_graphite', labelKey: 'capture.themeSolidGraphite' },
  { value: 'solid_carbon_black', labelKey: 'capture.themeSolidCarbonBlack' },
] as const;

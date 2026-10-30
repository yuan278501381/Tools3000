import type { FC, SVGAttributes } from 'react';

export interface LogoGlyphProps extends SVGAttributes<SVGSVGElement> {
  size?: number;
  className?: string;
  fill?: string;
  colorVariant?: boolean;
  themeAdaptive?: boolean;
}

/**
 * LogoGlyph — Tools3000 高精度矢量折带 T 标志 (Optical Sizing T-Ribbon Glyph)
 *
 * 1. 采用折带 T (t-ribbon) 核心造型：横梁由折叠带状结构形成，斜向竖干延续折面方向，极具前进感与速度感。
 * 2. 原生支持 fill="currentColor" 或 fill="var(--primary)"，与系统主题及动态主题色完美自适应。
 * 3. 采用分级折纸多重透明度光影映射 (1.0 / 0.88 / 0.72)，在单色/主题色填充下完整还原 3D 丝带流转立体质感。
 * 4. 原生支持 colorVariant 纯正色彩模式（#FF8B62 / #F15B42 / #D64132）。
 * 5. 原生支持 themeAdaptive 模式，联动暗色（#F8F9FB）与亮色（#1B1D23）背景高对比度呈现。
 */
export const LogoGlyph: FC<LogoGlyphProps> = ({
  size = 24,
  className = '',
  fill = 'currentColor',
  colorVariant = false,
  themeAdaptive = false,
  style,
  ...props
}) => {
  const isColor = colorVariant || fill === 'none';
  const computedClass = [
    'logo-glyph',
    themeAdaptive ? 'logo-glyph--theme-adaptive' : '',
    className,
  ]
    .filter(Boolean)
    .join(' ');

  return (
    <svg
      xmlns="http://www.w3.org/2000/svg"
      viewBox="0 0 64 64"
      width={size}
      height={size}
      className={computedClass}
      data-theme-adaptive={themeAdaptive ? 'true' : undefined}
      style={{ display: 'inline-block', flexShrink: 0, verticalAlign: 'middle', ...style }}
      fill="none"
      aria-hidden="true"
      {...props}
    >
      {/* 1. 基础轮廓体 (Base Ribbon Body) */}
      <path
        d="M3 11L48 3L61 15L43 23L39 56L23 62L27 27L5 30Z"
        fill={isColor ? '#F15B42' : fill}
        fillOpacity={isColor ? 1 : 0.88}
      />
      {/* 2. 顶部折叠受光高亮切面 (Top Ribbon Highlight Facet) */}
      <path
        d="M3 11L48 3L61 15L16 23Z"
        fill={isColor ? '#FF8B62' : fill}
        fillOpacity={isColor ? 1 : 1.0}
      />
      {/* 3. 竖干折叠阴影切面 (Stem Ribbon Shadow Facet) */}
      <path
        d="M27 27L43 23L39 56L23 62Z"
        fill={isColor ? '#D64132' : fill}
        fillOpacity={isColor ? 1 : 0.72}
      />
    </svg>
  );
};

/** 别名导出 */
export const Tools3000Logo: FC<LogoGlyphProps> = LogoGlyph;
export const TRibbonLogo: FC<LogoGlyphProps> = LogoGlyph;
export const Tools3000Bolt: FC<LogoGlyphProps> = LogoGlyph;

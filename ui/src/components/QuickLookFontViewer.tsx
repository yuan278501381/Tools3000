import { useEffect, useMemo, useState, type FC } from 'react';
import { useTranslation } from 'react-i18next';
import { ExternalLink, Type } from 'lucide-react';
import { bridgeRequest } from '../hooks/useBridge';

export interface FontAxisItem {
  tag: string;
  min: number;
  default: number;
  max: number;
}

export interface FontPreviewPayload {
  ok: boolean;
  format: string;
  familyName: string;
  subfamilyName?: string;
  fullName: string;
  version?: string;
  copyright?: string;
  designer?: string;
  glyphCount?: number;
  weightClass?: number;
  isItalic?: boolean;
  isVariable?: boolean;
  hasCjkSupport?: boolean;
  faceCount?: number;
  faceNames?: string[];
  axes?: FontAxisItem[];
}

interface QuickLookFontViewerProps {
  filePath: string;
  mediaUrl?: string;
  fontData: FontPreviewPayload;
}

const WATERFALL_SIZES = [14, 18, 24, 34, 48];

export const QuickLookFontViewer: FC<QuickLookFontViewerProps> = ({
  filePath,
  mediaUrl,
  fontData,
}) => {
  const { t } = useTranslation();
  const [activeFaceIndex, setActiveFaceIndex] = useState(0);
  const [fontLoadState, setFontLoadState] = useState<{
    url: string;
    family: string;
    failed: boolean;
  }>({ url: '', family: '', failed: false });

  const defaultSample = useMemo<string>(() => {
    if (fontData.hasCjkSupport) {
      return t(
        'quicklook.fontDefaultSampleCjk',
        'The quick brown fox jumps over 0123456789 - CJK & Latin Specimen',
      );
    }
    return t(
      'quicklook.fontDefaultSampleLatin',
      'Sphinx of black quartz, judge my vow - 0123456789 !@#$%&*()',
    );
  }, [fontData.hasCjkSupport, t]);

  const [customSampleState, setCustomSampleState] = useState<{
    base: string;
    text: string;
  } | null>(null);
  const sampleText =
    customSampleState && customSampleState.base === defaultSample
      ? customSampleState.text
      : defaultSample;
  const [axisValues, setAxisValues] = useState<Record<string, number>>(() => {
    const init: Record<string, number> = {};
    for (const ax of fontData.axes || []) {
      init[ax.tag] = ax.default;
    }
    return init;
  });

  const resolvedFontUrl = useMemo(() => {
    if (!mediaUrl) return '';
    if (fontData.format !== 'TTC') return mediaUrl;
    try {
      const u = new URL(mediaUrl);
      u.searchParams.set('f', String(activeFaceIndex));
      return u.toString();
    } catch {
      return mediaUrl;
    }
  }, [mediaUrl, fontData.format, activeFaceIndex]);

  const loadedFamily = fontLoadState.url === resolvedFontUrl ? fontLoadState.family : '';
  const loadFailed = fontLoadState.url === resolvedFontUrl ? fontLoadState.failed : false;

  useEffect(() => {
    if (!resolvedFontUrl || typeof FontFace === 'undefined') return;
    let cancelled = false;
    const uniqueName = `Tools3000QLFont_${Date.now()}_${activeFaceIndex}`;
    const face = new FontFace(uniqueName, `url("${resolvedFontUrl}")`);

    face
      .load()
      .then((loaded) => {
        if (cancelled) return;
        document.fonts.add(loaded);
        setFontLoadState({ url: resolvedFontUrl, family: uniqueName, failed: false });
      })
      .catch(() => {
        if (!cancelled) {
          setFontLoadState({ url: resolvedFontUrl, family: '', failed: true });
        }
      });

    return () => {
      cancelled = true;
      try {
        document.fonts.delete(face);
      } catch {
        // Ignore cleanup errors in synthetic environments
      }
    };
  }, [resolvedFontUrl, activeFaceIndex]);

  const variationSettings = useMemo(() => {
    const entries = Object.entries(axisValues);
    if (entries.length === 0) return undefined;
    return entries.map(([tag, val]) => `"${tag}" ${val}`).join(', ');
  }, [axisValues]);

  const previewStyle = useMemo(
    () => ({
      fontFamily: loadedFamily ? `"${loadedFamily}", var(--font-sans)` : 'var(--font-sans)',
      fontVariationSettings: variationSettings,
    }),
    [loadedFamily, variationSettings],
  );

  return (
    <div className="ql-font-view">
      <div className="ql-font-header-card">
        <div className="ql-font-title-block">
          <div className="ql-font-title-row">
            <Type size={18} aria-hidden="true" />
            <span className="ql-font-family-name">{fontData.fullName || fontData.familyName}</span>
            <span className="ql-archive-pill">{fontData.format}</span>
            {fontData.isVariable && (
              <span className="ql-archive-pill">{t('quicklook.fontVariableBadge', 'Variable')}</span>
            )}
          </div>
          <div className="ql-font-meta-row">
            {fontData.subfamilyName && <span>{fontData.subfamilyName}</span>}
            {typeof fontData.glyphCount === 'number' && fontData.glyphCount > 0 && (
              <span>· {t('quicklook.fontGlyphCount', '{{count}} glyphs', { count: fontData.glyphCount })}</span>
            )}
            {fontData.version && <span>· {fontData.version}</span>}
            {fontData.designer && <span>· {fontData.designer}</span>}
          </div>
        </div>

        <button
          type="button"
          className="ql-btn ql-btn-secondary"
          onClick={() => void bridgeRequest('quicklook.open', { path: filePath })}
        >
          <ExternalLink size={13} aria-hidden="true" />
          <span>{t('quicklook.openInDefaultApp', 'Open with Default App (Enter)')}</span>
        </button>
      </div>

      {fontData.faceNames && fontData.faceNames.length > 1 && (
        <div className="ql-font-faces-bar" role="tablist">
          {fontData.faceNames.map((faceName, idx) => (
            <button
              key={`${faceName}-${idx}`}
              type="button"
              role="tab"
              aria-selected={activeFaceIndex === idx}
              className={`ql-btn-sub ${activeFaceIndex === idx ? 'active' : ''}`}
              onClick={() => setActiveFaceIndex(idx)}
            >
              {faceName}
            </button>
          ))}
        </div>
      )}

      <div className="ql-font-controls">
        <input
          type="text"
          className="ql-font-sample-input"
          value={sampleText}
          onChange={(e) => setCustomSampleState({ base: defaultSample, text: e.target.value })}
          placeholder={t('quicklook.fontTryTypingPlaceholder', 'Type custom text to preview this font...')}
          aria-label={t('quicklook.fontTryTypingPlaceholder', 'Type custom text to preview this font...')}
        />
        {fontData.axes && fontData.axes.length > 0 && (
          <div className="ql-font-axes">
            {fontData.axes.map((ax) => {
              const val = axisValues[ax.tag] ?? ax.default;
              return (
                <label key={ax.tag} className="ql-font-axis-item">
                  <span className="ql-font-axis-label">{ax.tag}: {Math.round(val)}</span>
                  <input
                    type="range"
                    min={ax.min}
                    max={ax.max}
                    step={(ax.max - ax.min) > 20 ? 1 : 0.1}
                    value={val}
                    onChange={(e) => setAxisValues((prev) => ({ ...prev, [ax.tag]: Number(e.target.value) }))}
                  />
                </label>
              );
            })}
          </div>
        )}
      </div>

      {loadFailed ? (
        <div className="ql-font-error">
          {t('quicklook.fontRenderFailed', 'Unable to render this font file directly in preview. Press Enter to inspect it in Windows Font Viewer.')}
        </div>
      ) : (
        <div className="ql-font-waterfall">
          {WATERFALL_SIZES.map((px) => (
            <div key={px} className="ql-font-waterfall-row">
              <span className="ql-font-size-tag">{px}px</span>
              <div
                className="ql-font-sample-line"
                style={{ ...previewStyle, fontSize: `${px}px` }}
              >
                {sampleText || defaultSample}
              </div>
            </div>
          ))}
          <div className="ql-font-charset-card" style={previewStyle}>
            <div>ABCDEFGHIJKLMNOPQRSTUVWXYZ</div>
            <div>abcdefghijklmnopqrstuvwxyz</div>
            <div>0123456789 !@#$%^&amp;*()_+-=[]&#123;&#125;;:&apos;&quot;,.&lt;&gt;/?</div>
          </div>
        </div>
      )}

      {fontData.copyright && (
        <div className="ql-font-copyright">{fontData.copyright}</div>
      )}
    </div>
  );
};

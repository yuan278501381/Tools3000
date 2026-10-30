import React, { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import './GestureGuide.css';

const GUIDES = [
  { code: 'U', nameKey: 'gestureGuide.dirU', path: 'M 50 80 L 50 20' },
  { code: 'D', nameKey: 'gestureGuide.dirD', path: 'M 50 20 L 50 80' },
  { code: 'L', nameKey: 'gestureGuide.dirL', path: 'M 80 50 L 20 50' },
  { code: 'R', nameKey: 'gestureGuide.dirR', path: 'M 20 50 L 80 50' },
  { code: 'DR', nameKey: 'gestureGuide.dirDR', path: 'M 30 30 L 70 70' },
  { code: 'DL', nameKey: 'gestureGuide.dirDL', path: 'M 70 30 L 30 70' },
  { code: 'UR', nameKey: 'gestureGuide.dirUR', path: 'M 30 70 L 70 30' },
  { code: 'UL', nameKey: 'gestureGuide.dirUL', path: 'M 70 70 L 30 30' },
  { code: 'RD', nameKey: 'gestureGuide.dirRD', path: 'M 30 30 L 70 30 L 70 70' },
] as const;

export const GestureGuide: React.FC = () => {
  const { t } = useTranslation();
  const [index, setIndex] = useState(0);

  useEffect(() => {
    const timer = setInterval(() => {
      setIndex((i) => (i + 1) % GUIDES.length);
    }, 2500);
    return () => clearInterval(timer);
  }, []);

  const guide = GUIDES[index];

  return (
    <div className="gesture-guide">
      <div className="gesture-guide__info">
        <div className="gesture-guide__title">{t('gestureGuide.title')}</div>
        <div className="gesture-guide__desc">{t('gestureGuide.desc')}</div>
        <div className="gesture-guide__badge">
          <span>{guide.code}</span>
          <span>{t(guide.nameKey)}</span>
        </div>
      </div>
      <div className="gesture-guide__canvas">
        <svg viewBox="0 0 100 100" width="100%" height="100%">
          <path
            key={guide.code}
            className="gesture-guide__path"
            d={guide.path}
            fill="none"
            stroke="var(--primary)"
            strokeWidth="6"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
          {/* Animated dot */}
          <circle 
            className="gesture-guide__dot" 
            r="4" 
            fill="var(--primary)" 
            style={{ offsetPath: `path('${guide.path}')` } as React.CSSProperties}
          />
        </svg>
      </div>
    </div>
  );
};

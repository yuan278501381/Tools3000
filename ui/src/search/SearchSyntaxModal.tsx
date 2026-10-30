import React from 'react';
import { useTranslation } from 'react-i18next';
import { Sparkles, X, CornerDownLeft } from 'lucide-react';
import { CodeBadge } from '../components/UIKit';
import { SYNTAX_CATEGORIES, SYNTAX_EXAMPLES } from './searchConstants';

export interface SearchSyntaxModalProps {
  visible: boolean;
  onClose: () => void;
  syntaxCat: string;
  onSelectCategory: (catId: string) => void;
  onApplyExample: (syntax: string) => void;
}

export const SearchSyntaxModal: React.FC<SearchSyntaxModalProps> = ({
  visible,
  onClose,
  syntaxCat,
  onSelectCategory,
  onApplyExample,
}) => {
  const { t } = useTranslation();

  if (!visible) return null;

  const filteredSyntaxExamples = syntaxCat === 'all'
    ? SYNTAX_EXAMPLES
    : SYNTAX_EXAMPLES.filter(item => item.category === syntaxCat);

  return (
    <div className="search-syntax-drawer">
      <div className="syntax-drawer-header">
        <div className="syntax-drawer-header-left">
          <div className="syntax-drawer-title">
            <Sparkles size={16} className="syntax-drawer-title-icon" />
            <span>{t('search.syntaxHelpTitle', 'Advanced Search Syntax Quick Guide')}</span>
            <span className="syntax-drawer-kbd"><kbd>F1</kbd></span>
          </div>
          <div className="syntax-drawer-tabs">
            {SYNTAX_CATEGORIES.map(cat => (
              <button
                key={cat.id}
                type="button"
                className={`syntax-drawer-tab ${syntaxCat === cat.id ? 'active' : ''}`}
                onClick={() => onSelectCategory(cat.id)}
              >
                {t(cat.labelKey as unknown as 'search.catAll', cat.defaultLabel)}
              </button>
            ))}
          </div>
        </div>
        <button
          className="syntax-drawer-close"
          onClick={onClose}
          type="button"
          title={t('search.closeSyntaxHelp', 'Close Syntax Guide (Esc / F1)')}
        >
          <X size={16} />
        </button>
      </div>
      <div className="syntax-drawer-list">
        {filteredSyntaxExamples.map((item, idx) => (
          <div
            key={idx}
            className={`syntax-example-item ${item.highlight ? 'syntax-example-item--highlight' : ''}`}
            onClick={() => onApplyExample(item.syntax)}
            title={t('search.clickToFill', 'Click to insert into search box')}
          >
            <div className="syntax-example-item-top">
              <CodeBadge className="syntax-code" variant={item.highlight ? 'primary' : 'default'}>{item.syntax}</CodeBadge>
              <span className="syntax-example-action-hint">
                <span>{t('search.fillInHint', 'Insert ↵').replace(/\s*↵\s*$/u, '')}</span>
                <CornerDownLeft size={12} strokeWidth={2.2} aria-hidden="true" />
              </span>
            </div>
            <span className="syntax-desc">{t(item.descKey as unknown as 'search.catAll', item.defaultDesc)}</span>
          </div>
        ))}
      </div>
    </div>
  );
};

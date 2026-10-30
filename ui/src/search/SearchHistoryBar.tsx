import React from 'react';
import { useTranslation } from 'react-i18next';
import { Clock, Trash2, X } from 'lucide-react';

export interface SearchHistoryBarProps {
  query: string;
  searchHistory: Array<{ search: string; searchCount: number }>;
  onClearAllHistory: () => void;
  onSelectHistoryItem: (text: string) => void;
  onRemoveHistoryItem: (e: React.MouseEvent, text: string) => void;
}

export const SearchHistoryBar: React.FC<SearchHistoryBarProps> = ({
  query,
  searchHistory,
  onClearAllHistory,
  onSelectHistoryItem,
  onRemoveHistoryItem,
}) => {
  const { t } = useTranslation();

  if (query.trim() !== '' || searchHistory.length === 0) {
    return null;
  }

  return (
    <div className="search-history-container">
      <div className="search-history-header">
        <div className="search-history-title">
          <Clock size={13} />
          <span>{t('search.recentSearches', 'Recent Search History')}</span>
        </div>
        <button
          type="button"
          className="search-history-clear-btn"
          onClick={onClearAllHistory}
          title={t('search.clearHistory', 'Clear History')}
        >
          <Trash2 size={11} />
          <span>{t('search.clearHistory', 'Clear History')}</span>
        </button>
      </div>
      <div className="search-history-chips">
        {searchHistory.map((item, idx) => (
          <div
            key={idx}
            className="search-history-chip"
            onClick={() => onSelectHistoryItem(item.search)}
            title={t('search.searchCountTip', 'Search frequency: {{count}} times · Click to reuse', { count: item.searchCount })}
          >
            <Clock size={11} className="search-history-chip-icon" />
            <span className="search-history-chip-text">{item.search}</span>
            {item.searchCount > 1 && (
              <span className="search-history-chip-count">{item.searchCount}</span>
            )}
            <button
              type="button"
              className="search-history-chip-del"
              onClick={(e) => onRemoveHistoryItem(e, item.search)}
              title={t('search.deleteHistoryItem', 'Delete this history')}
            >
              <X size={10} />
            </button>
          </div>
        ))}
      </div>
    </div>
  );
};

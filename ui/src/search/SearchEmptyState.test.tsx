/* @vitest-environment jsdom */

import React from 'react';
import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, optsOrDefault?: string | { defaultValue?: string }) => {
      if (typeof optsOrDefault === 'string') return optsOrDefault;
      if (optsOrDefault && typeof optsOrDefault === 'object' && typeof optsOrDefault.defaultValue === 'string') {
        return optsOrDefault.defaultValue;
      }
      return _key;
    },
  }),
}));

import { SearchEmptyState } from './SearchEmptyState';

describe('SearchEmptyState anti-flash snapshotQuery guard', () => {
  const baseProps = {
    isInitialIndexing: false,
    isServiceStarting: false,
    serviceAvailable: true,
    dbStats: null,
    loading: false,
    activeCategory: 'all',
    searchMode: 'name' as const,
    sortedResults: [],
    searchHistoryLength: 0,
    scanElapsedSeconds: 0,
    lastColdScanDurationSec: 10,
    actionError: '',
    categories: [
      { id: 'all', label: 'All', prefix: '' },
      { id: 'content', label: 'Content', prefix: 'content:' },
    ],
    onSelectCategory: vi.fn(),
    onUpdateQuery: vi.fn(),
    inputRef: React.createRef<HTMLInputElement>(),
  };

  it('suppresses "No files containing ..." while snapshotQuery does not match current typed query', () => {
    const { container } = render(
      <SearchEmptyState
        {...baseProps}
        query="wechat"
        snapshotQuery=""
      />,
    );

    expect(container.textContent).toBe('');
  });

  it('renders "No files containing ..." once snapshotQuery matches current typed query with zero results', () => {
    render(
      <SearchEmptyState
        {...baseProps}
        query="nonexistent_app_xyz"
        snapshotQuery="nonexistent_app_xyz"
      />,
    );

    expect(screen.getByText(/No files containing "nonexistent_app_xyz"/)).toBeTruthy();
  });
});

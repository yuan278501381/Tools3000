/* @vitest-environment jsdom */

import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { SearchExcludedPathsManager } from './SearchExcludedPathsManager';
import {
  MAX_CUSTOM_EXCLUDED_PATHS,
  mergeExcludedPaths,
  normalizeExcludedPath,
  parseExcludedPathInput,
} from '../search/excludedPathsUtils';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (_key: string, fallback?: string, opts?: Record<string, unknown>) => {
      if (!fallback) return _key;
      if (!opts) return fallback;
      return fallback.replace(/\{\{(\w+)\}\}/g, (_, k) => String(opts[k] ?? ''));
    },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    info: vi.fn(),
    warning: vi.fn(),
    error: vi.fn(),
  },
}));

describe('SearchExcludedPathsManager path utilities', () => {
  it('normalizes slashes, quotes, drive roots, and trailing separators', () => {
    expect(normalizeExcludedPath('  "D:/Movies/Action/"  ')).toBe('D:\\Movies\\Action');
    expect(normalizeExcludedPath('d:')).toBe('D:\\');
    expect(normalizeExcludedPath('e:///')).toBe('E:\\');
    expect(normalizeExcludedPath('\\\\nas\\media\\clips\\')).toBe('\\\\nas\\media\\clips');
    expect(normalizeExcludedPath('   ')).toBe('');
    expect(normalizeExcludedPath('\\')).toBe('');
  });

  it('splits multi-path pastes by newline or semicolon while preserving spaces and commas', () => {
    const parsed = parseExcludedPathInput(
      'D:\\My Videos, 2026\\Clips; "E:/Archive/Secret Docs"\nD:\\my videos, 2026\\clips\\',
    );
    expect(parsed).toEqual([
      'D:\\My Videos, 2026\\Clips',
      'E:\\Archive\\Secret Docs',
    ]);
  });

  it('deduplicates case-insensitively and enforces max capacity', () => {
    const base = Array.from({ length: MAX_CUSTOM_EXCLUDED_PATHS - 1 }, (_, i) => `D:\\Folder_${i}`);
    const merged = mergeExcludedPaths(base, [
      'd:\\folder_0\\',
      'D:\\New_A',
      'D:\\New_B',
    ]);
    expect(merged.duplicateCount).toBe(1);
    expect(merged.added).toEqual(['D:\\New_A']);
    expect(merged.capped).toBe(true);
    expect(merged.paths).toHaveLength(MAX_CUSTOM_EXCLUDED_PATHS);
  });
});

describe('SearchExcludedPathsManager component', () => {
  it('supports adding via folder picker, multi-path input, per-item remove, and clear all', async () => {
    const onChange = vi.fn();
    const onBrowse = vi.fn().mockResolvedValue('E:\\PrivateVault\\');

    const { rerender } = render(
      <SearchExcludedPathsManager
        paths={['D:\\Movies']}
        onChange={onChange}
        onBrowse={onBrowse}
      />,
    );

    expect(screen.getByText('D:\\Movies')).toBeTruthy();

    fireEvent.click(screen.getByText('Select Folder'));
    await waitFor(() => {
      expect(onBrowse).toHaveBeenCalledTimes(1);
      expect(onChange).toHaveBeenCalledWith(['D:\\Movies', 'E:\\PrivateVault']);
    });

    onChange.mockClear();
    rerender(
      <SearchExcludedPathsManager
        paths={['D:\\Movies', 'E:\\PrivateVault']}
        onChange={onChange}
        onBrowse={onBrowse}
      />,
    );

    const input = screen.getByLabelText('Folder path to exclude') as HTMLInputElement;
    fireEvent.change(input, { target: { value: 'F:/Media/Clips; G:\\Backup\\Old' } });
    fireEvent.keyDown(input, { key: 'Enter' });

    expect(onChange).toHaveBeenCalledWith([
      'D:\\Movies',
      'E:\\PrivateVault',
      'F:\\Media\\Clips',
      'G:\\Backup\\Old',
    ]);

    onChange.mockClear();
    fireEvent.click(screen.getByLabelText('Remove excluded folder: D:\\Movies'));
    expect(onChange).toHaveBeenCalledWith(['E:\\PrivateVault']);

    onChange.mockClear();
    fireEvent.click(screen.getByText('Clear All'));
    expect(onChange).toHaveBeenCalledWith([]);
  });
});

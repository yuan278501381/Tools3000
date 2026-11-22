/* @vitest-environment jsdom */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { SearchExternalApiCard } from './SearchExternalApiCard';

const bridgeRequestMock = vi.fn();

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, fallback?: string) => fallback || key,
    i18n: { language: 'en', resolvedLanguage: 'en' },
  }),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
  },
}));

describe('SearchExternalApiCard', () => {
  afterEach(() => {
    cleanup();
  });

  beforeEach(() => {
    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockResolvedValue({
      installed: true,
      executablePath: 'C:\\Program Files\\Tools3000\\tools3000-cli.exe',
      version: '1.4.0',
    });
  });

  it('renders disabled-by-default security badge and saves toggle updates', async () => {
    const onSave = vi.fn().mockResolvedValue(undefined);
    render(
      <SearchExternalApiCard
        externalApiEnabled={false}
        externalApiExcludePaths=""
        externalApiAllowedRoots=""
        onSaveSettings={onSave}
      />,
    );

    expect(screen.getByText('Disabled by Default')).toBeTruthy();
    await waitFor(() => {
      expect(screen.getByText('C:\\Program Files\\Tools3000\\tools3000-cli.exe')).toBeTruthy();
    });

    const toggle = document.getElementById('search-external-api-toggle') as HTMLInputElement;
    expect(toggle).toBeTruthy();
    fireEvent.click(toggle);

    await waitFor(() => {
      expect(onSave).toHaveBeenCalledWith({ externalApiEnabled: true });
    });
  });

  it('switches integration preset tabs cleanly', async () => {
    const onSave = vi.fn().mockResolvedValue(undefined);
    render(
      <SearchExternalApiCard
        externalApiEnabled={true}
        externalApiExcludePaths="D:\\Private"
        externalApiAllowedRoots="C:\\repo"
        onSaveSettings={onSave}
      />,
    );

    expect(screen.getByText('Read-Only Active')).toBeTruthy();
    const claudeTab = screen.getByRole('tab', { name: 'Claude Code CLI' });
    fireEvent.click(claudeTab);
    await waitFor(() => {
      expect(screen.getByText(/claude mcp add tools3000-search/)).toBeTruthy();
    });
  });
});

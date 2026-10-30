/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import * as bridge from './hooks/useBridge';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string) => key,
    i18n: {
      language: 'zh-CN',
      changeLanguage: vi.fn().mockResolvedValue(undefined),
    },
  }),
}));

// Mock window.matchMedia
Object.defineProperty(window, 'matchMedia', {
  writable: true,
  value: vi.fn().mockImplementation((query: string) => ({
    matches: false,
    media: query,
    onchange: null,
    addListener: vi.fn(),
    removeListener: vi.fn(),
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    dispatchEvent: vi.fn(),
  })),
});

describe('App Onboarding Integration', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    localStorage.clear();
  });

  afterEach(() => {
    cleanup();
  });

  it('shows onboarding modal on fresh install when config keys do not exist (null)', async () => {
    const bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: unknown) => {
      const p = params as { key?: string } | undefined;
      if (method === 'config.get') {
        if (p?.key === '/app/onboardingCompleted') return null;
        if (p?.key === '/general/showOnboarding') return null;
      }
      if (method === 'plugins.getAll') {
        return [{ id: 'search', name: 'Search', enabled: true, active: true }];
      }
      return {};
    });

    await act(async () => {
      render(<App />);
    });

    // Onboarding modal should be visible
    expect(screen.getByText('onboarding.welcomeTitle')).toBeTruthy();
    expect(screen.getByText('onboarding.skip')).toBeTruthy();
    bridgeSpy.mockRestore();
  });

  it('does NOT show onboarding modal when user previously completed it', async () => {
    const bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: unknown) => {
      const p = params as { key?: string } | undefined;
      if (method === 'config.get') {
        if (p?.key === '/app/onboardingCompleted') return true;
        if (p?.key === '/general/showOnboarding') return false;
      }
      if (method === 'plugins.getAll') {
        return [{ id: 'search', name: 'Search', enabled: true, active: true }];
      }
      return {};
    });

    await act(async () => {
      render(<App />);
    });

    expect(screen.queryByText('onboarding.welcomeTitle')).toBeNull();
    bridgeSpy.mockRestore();
  });

  it('shows onboarding modal when explicitShow is true even if completed is true', async () => {
    const bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: unknown) => {
      const p = params as { key?: string } | undefined;
      if (method === 'config.get') {
        if (p?.key === '/app/onboardingCompleted') return true;
        if (p?.key === '/general/showOnboarding') return true;
      }
      if (method === 'plugins.getAll') {
        return [{ id: 'search', name: 'Search', enabled: true, active: true }];
      }
      return {};
    });

    await act(async () => {
      render(<App />);
    });

    expect(screen.getByText('onboarding.welcomeTitle')).toBeTruthy();
    bridgeSpy.mockRestore();
  });

  it('persists completion state and hides modal when onboarding is skipped or completed', async () => {
    const bridgeSpy = vi.spyOn(bridge, 'bridgeRequest').mockImplementation(async (method: string, params?: unknown) => {
      const p = params as { key?: string } | undefined;
      if (method === 'config.get') {
        if (p?.key === '/app/onboardingCompleted') return null;
        if (p?.key === '/general/showOnboarding') return null;
      }
      if (method === 'plugins.getAll') {
        return [{ id: 'search', name: 'Search', enabled: true, active: true }];
      }
      return { success: true };
    });

    await act(async () => {
      render(<App />);
    });

    const skipButton = screen.getByText('onboarding.skip');
    expect(skipButton).toBeTruthy();

    await act(async () => {
      fireEvent.click(skipButton);
    });

    // Verify modal is closed
    expect(screen.queryByText('onboarding.welcomeTitle')).toBeNull();

    // Verify completion was persisted to backend
    expect(bridgeSpy).toHaveBeenCalledWith('config.set', { key: '/app/onboardingCompleted', value: true });
    expect(bridgeSpy).toHaveBeenCalledWith('config.set', { key: '/general/showOnboarding', value: false });
    expect(bridgeSpy).toHaveBeenCalledWith('general.updateSettings', { showOnboarding: false }, { silent: true });

    bridgeSpy.mockRestore();
  });
});

/* @vitest-environment jsdom */

import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { OnboardingModal } from './OnboardingModal';

// Mock i18next
vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, options?: Record<string, unknown>) => {
      if (options?.current && options?.total) {
        return `Step ${options.current} of ${options.total}`;
      }
      return key;
    },
  }),
}));

describe('OnboardingModal', () => {
  const onCompleteMock = vi.fn();

  beforeEach(() => {
    vi.clearAllMocks();
  });

  afterEach(() => {
    cleanup();
  });

  it('renders Step 0 with exactly 7 core capability feature cards, including Remote Boost', () => {
    const { container } = render(<OnboardingModal onComplete={onCompleteMock} />);

    // Step 0 header
    expect(screen.getByText('onboarding.welcomeTitle')).toBeTruthy();
    expect(screen.getByText('onboarding.welcomeSubtitle')).toBeTruthy();

    // 7 Core features inside .onboarding__features container
    const featuresContainer = container.querySelector('.onboarding__features');
    expect(featuresContainer).toBeTruthy();

    const cards = featuresContainer?.querySelectorAll('.onboarding__feature-card');
    expect(cards?.length).toBe(7);

    expect(screen.getByText('onboarding.featureSearch')).toBeTruthy();
    expect(screen.getByText('onboarding.featureCapture')).toBeTruthy();
    expect(screen.getAllByText('onboarding.featureGesture').length).toBeGreaterThanOrEqual(1);
    expect(screen.getByText('onboarding.featureKeycast')).toBeTruthy();
    expect(screen.getByText('onboarding.featureDialogEnhancer')).toBeTruthy();
    expect(screen.getByText('onboarding.featureSpotlight')).toBeTruthy();
    expect(screen.getByText('onboarding.featureRemoteBoost')).toBeTruthy();
    expect(screen.getByText('onboarding.featureRemoteBoostDesc')).toBeTruthy();
  });

  it('navigates through steps to shortcuts and showcase, verifying Remote Boost entries and tab switching', async () => {
    render(<OnboardingModal onComplete={onCompleteMock} />);

    // Step 0 -> Step 1 (Shortcuts)
    const nextButton = screen.getByRole('button', { name: /onboarding\.next/i });
    await act(async () => {
      fireEvent.click(nextButton);
    });

    expect(screen.getByText('onboarding.shortcutsTitle')).toBeTruthy();
    expect(screen.getByText('onboarding.shortcutRemoteFlush')).toBeTruthy();
    expect(screen.getByText('Ctrl + Alt + Backspace')).toBeTruthy();

    // Step 1 -> Step 2 (Gestures & Showcase)
    await act(async () => {
      fireEvent.click(nextButton);
    });

    expect(screen.getByText('onboarding.gestureTitle')).toBeTruthy();
    expect(screen.getByText('onboarding.tabSpotlight')).toBeTruthy();
    expect(screen.getByText('onboarding.tabRemote')).toBeTruthy();

    // Initial showcase tab is spotlight
    expect(screen.getByText('onboarding.spotlightItem1')).toBeTruthy();

    // Switch tab to Remote Boost
    const remoteTab = screen.getByRole('tab', { name: /onboarding\.tabRemote/i });
    await act(async () => {
      fireEvent.click(remoteTab);
    });

    expect(screen.getByText('onboarding.remoteItem1')).toBeTruthy();
    expect(screen.getByText('onboarding.remoteItem2')).toBeTruthy();
    expect(screen.getByText('onboarding.remoteItem3')).toBeTruthy();
    expect(screen.getByText('onboarding.remoteItem4')).toBeTruthy();

    // Step 2 -> Step 3 (Complete)
    await act(async () => {
      fireEvent.click(nextButton);
    });

    expect(screen.getByText('onboarding.completeTitle')).toBeTruthy();
    const startButton = screen.getByRole('button', { name: /onboarding\.startButton/i });
    await act(async () => {
      fireEvent.click(startButton);
    });

    expect(onCompleteMock).toHaveBeenCalledTimes(1);
  });

  it('supports backwards navigation via the previous button', async () => {
    render(<OnboardingModal onComplete={onCompleteMock} />);

    const nextButton = screen.getByRole('button', { name: /onboarding\.next/i });
    await act(async () => {
      fireEvent.click(nextButton);
    });
    expect(screen.getByText('onboarding.shortcutsTitle')).toBeTruthy();

    const prevButton = screen.getByRole('button', { name: /onboarding\.prev/i });
    await act(async () => {
      fireEvent.click(prevButton);
    });
    expect(screen.getByText('onboarding.welcomeTitle')).toBeTruthy();
  });

  it('navigates via dot indicator buttons', async () => {
    render(<OnboardingModal onComplete={onCompleteMock} />);

    const step3Dot = screen.getByRole('button', { name: 'Step 3 of 4' });
    await act(async () => {
      fireEvent.click(step3Dot);
    });
    expect(screen.getByText('onboarding.gestureTitle')).toBeTruthy();
  });

  it('navigates forward and backward with arrow keys, and closes with Escape', async () => {
    render(<OnboardingModal onComplete={onCompleteMock} />);

    // ArrowRight advances to Step 1
    await act(async () => {
      fireEvent.keyDown(window, { key: 'ArrowRight' });
    });
    expect(screen.getByText('onboarding.shortcutsTitle')).toBeTruthy();

    // ArrowLeft retreats to Step 0
    await act(async () => {
      fireEvent.keyDown(window, { key: 'ArrowLeft' });
    });
    expect(screen.getByText('onboarding.welcomeTitle')).toBeTruthy();

    // Escape triggers onComplete
    await act(async () => {
      fireEvent.keyDown(window, { key: 'Escape' });
    });
    expect(onCompleteMock).toHaveBeenCalledTimes(1);
  });

  it('switches showcase tabs using arrow keys when a tab is focused', async () => {
    render(<OnboardingModal onComplete={onCompleteMock} />);

    // Navigate to Step 2
    const step2Dot = screen.getByRole('button', { name: 'Step 3 of 4' });
    await act(async () => {
      fireEvent.click(step2Dot);
    });

    const spotlightTab = screen.getByRole('tab', { name: /onboarding\.tabSpotlight/i });
    spotlightTab.focus();

    // ArrowRight while focusing tab switches to Remote tab
    await act(async () => {
      fireEvent.keyDown(window, { key: 'ArrowRight' });
    });
    expect(screen.getByText('onboarding.remoteItem1')).toBeTruthy();

    // ArrowLeft switches back to Spotlight tab
    const remoteTab = screen.getByRole('tab', { name: /onboarding\.tabRemote/i });
    remoteTab.focus();
    await act(async () => {
      fireEvent.keyDown(window, { key: 'ArrowLeft' });
    });
    expect(screen.getByText('onboarding.spotlightItem1')).toBeTruthy();
  });

  it('triggers onComplete when clicking the skip button', async () => {
    render(<OnboardingModal onComplete={onCompleteMock} />);

    const skipButton = screen.getByRole('button', { name: /onboarding\.skip/i });
    await act(async () => {
      fireEvent.click(skipButton);
    });

    expect(onCompleteMock).toHaveBeenCalledTimes(1);
  });

  it('triggers onComplete when clicking the backdrop, but not when clicking inside the card', async () => {
    const { container } = render(<OnboardingModal onComplete={onCompleteMock} />);

    const card = container.querySelector('.onboarding__card');
    expect(card).toBeTruthy();
    if (card) {
      await act(async () => {
        fireEvent.click(card);
      });
      expect(onCompleteMock).not.toHaveBeenCalled();
    }

    const backdrop = container.querySelector('.onboarding');
    expect(backdrop).toBeTruthy();
    if (backdrop) {
      await act(async () => {
        fireEvent.click(backdrop);
      });
      expect(onCompleteMock).toHaveBeenCalledTimes(1);
    }
  });

  it('stops propagation of mouse wheel events on the backdrop', () => {
    const { container } = render(<OnboardingModal onComplete={onCompleteMock} />);
    const backdrop = container.querySelector('.onboarding');
    expect(backdrop).toBeTruthy();
    if (backdrop) {
      const event = new Event('wheel', { bubbles: true, cancelable: true });
      const stopSpy = vi.spyOn(event, 'stopPropagation');
      fireEvent(backdrop, event);
      expect(stopSpy).toHaveBeenCalled();
    }
  });
});

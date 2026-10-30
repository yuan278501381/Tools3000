/* @vitest-environment jsdom */

import { describe, expect, it } from 'vitest';
import { shouldShowOnboarding } from './onboardingGate';

describe('onboardingGate', () => {
  it('shows onboarding on fresh install where neither flag is set', () => {
    expect(shouldShowOnboarding({})).toBe(true);
    expect(shouldShowOnboarding({ completed: false, explicitShow: false })).toBe(true);
    expect(shouldShowOnboarding({ completed: undefined, explicitShow: undefined })).toBe(true);
    expect(shouldShowOnboarding({ completed: null, explicitShow: null })).toBe(true);
    expect(shouldShowOnboarding({ completed: null, explicitShow: false })).toBe(true);
    expect(shouldShowOnboarding({ completed: false, explicitShow: null })).toBe(true);
  });

  it('hides onboarding when completed is true and explicitShow is not enabled', () => {
    expect(shouldShowOnboarding({ completed: true, explicitShow: false })).toBe(false);
    expect(shouldShowOnboarding({ completed: true, explicitShow: null })).toBe(false);
    expect(shouldShowOnboarding({ completed: true, explicitShow: undefined })).toBe(false);
    expect(shouldShowOnboarding({ completed: true })).toBe(false);
  });

  it('prioritizes explicitShow === true even if completed is true', () => {
    expect(shouldShowOnboarding({ completed: true, explicitShow: true })).toBe(true);
    expect(shouldShowOnboarding({ completed: false, explicitShow: true })).toBe(true);
    expect(shouldShowOnboarding({ completed: null, explicitShow: true })).toBe(true);
    expect(shouldShowOnboarding({ completed: undefined, explicitShow: true })).toBe(true);
    expect(shouldShowOnboarding({ explicitShow: true })).toBe(true);
  });

  it('shows onboarding if completed is false or missing regardless of explicitShow false', () => {
    expect(shouldShowOnboarding({ completed: false, explicitShow: false })).toBe(true);
    expect(shouldShowOnboarding({ completed: null, explicitShow: false })).toBe(true);
    expect(shouldShowOnboarding({ completed: undefined, explicitShow: false })).toBe(true);
    expect(shouldShowOnboarding({ completed: false })).toBe(true);
    expect(shouldShowOnboarding({ completed: null })).toBe(true);
  });
});

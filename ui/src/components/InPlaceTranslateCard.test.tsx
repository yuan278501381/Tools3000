/* @vitest-environment jsdom */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, render, screen, fireEvent, waitFor } from '@testing-library/react';
import { InPlaceTranslateCard } from './InPlaceTranslateCard';

const bridgeRequestMock = vi.fn();

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string) => key,
    i18n: { language: 'zh-CN' },
  }),
}));

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
  },
}));

describe('InPlaceTranslateCard', () => {
  afterEach(() => {
    cleanup();
  });

  beforeEach(() => {
    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'translate.getSettings') {
        return {
          provider: 'free_auto',
          targetLang: 'zh-Hans',
          secondaryTargetLang: 'en',
          freePrimary: 'edge',
          openaiBaseUrl: 'https://api.openai.com/v1',
          openaiModel: 'gpt-4o-mini',
          hasOpenaiApiKey: false,
          openaiApiKeyMasked: '',
          deeplEndpoint: '',
          hasDeeplApiKey: false,
          deeplApiKeyMasked: '',
          timeoutSec: 15,
        };
      }
      if (method === 'translate.updateSettings') {
        return {
          success: true,
          settings: {
            provider: (params?.provider as string) || 'openai',
            targetLang: 'zh-Hans',
            secondaryTargetLang: 'en',
            freePrimary: 'edge',
            openaiBaseUrl: (params?.openaiBaseUrl as string) || 'http://127.0.0.1:11434/v1',
            openaiModel: (params?.openaiModel as string) || 'qwen2.5:7b',
            hasOpenaiApiKey: Boolean(params?.openaiApiKey),
            openaiApiKeyMasked: params?.openaiApiKey ? 'sk-...1234' : '',
            deeplEndpoint: '',
            hasDeeplApiKey: false,
            deeplApiKeyMasked: '',
            timeoutSec: 15,
          },
        };
      }
      if (method === 'translate.testConnection') {
        return {
          success: true,
          providerUsed: 'edge_free',
          resolvedTargetLang: 'zh-Hans',
          elapsedMs: 142,
          sampleOutput: '你好，Tools3000！',
        };
      }
      return { success: true };
    });
  });

  it('loads zero-config free settings and runs on-demand connection test', async () => {
    render(<InPlaceTranslateCard />);

    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('translate.getSettings', undefined, { silent: true });
    });

    const testBtn = await screen.findByRole('button', { name: /translate\.testConnectionBtn/i });
    fireEvent.click(testBtn);

    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'translate.testConnection',
        expect.objectContaining({ provider: 'free_auto', targetLang: 'zh-Hans' }),
      );
    });

    expect(await screen.findByText(/142 ms/)).toBeTruthy();
    expect(await screen.findByText(/你好，Tools3000！/)).toBeTruthy();
  });

  it('switches to OpenAI-compatible provider and applies local Ollama offline preset', async () => {
    bridgeRequestMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
      if (method === 'translate.getSettings') {
        return {
          provider: 'openai',
          targetLang: 'zh-Hans',
          secondaryTargetLang: 'en',
          freePrimary: 'edge',
          openaiBaseUrl: 'https://api.openai.com/v1',
          openaiModel: 'gpt-4o-mini',
          hasOpenaiApiKey: true,
          openaiApiKeyMasked: 'sk-...9876',
          deeplEndpoint: '',
          hasDeeplApiKey: false,
          deeplApiKeyMasked: '',
          timeoutSec: 15,
        };
      }
      if (method === 'translate.updateSettings') {
        return {
          success: true,
          settings: {
            provider: 'openai',
            targetLang: 'zh-Hans',
            secondaryTargetLang: 'en',
            freePrimary: 'edge',
            openaiBaseUrl: (params?.openaiBaseUrl as string) || 'http://127.0.0.1:11434/v1',
            openaiModel: (params?.openaiModel as string) || 'qwen2.5:7b',
            hasOpenaiApiKey: true,
            openaiApiKeyMasked: 'sk-...9876',
            deeplEndpoint: '',
            hasDeeplApiKey: false,
            deeplApiKeyMasked: '',
            timeoutSec: 15,
          },
        };
      }
      return { success: true };
    });

    render(<InPlaceTranslateCard />);

    expect(await screen.findByText('sk-...9876')).toBeTruthy();

    const ollamaBtn = await screen.findByRole('button', { name: /translate\.presetOllama/i });
    fireEvent.click(ollamaBtn);

    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'translate.updateSettings',
        expect.objectContaining({
          provider: 'openai',
          openaiBaseUrl: 'http://127.0.0.1:11434/v1',
          openaiModel: 'qwen2.5:7b',
        }),
      );
    });
  });
});

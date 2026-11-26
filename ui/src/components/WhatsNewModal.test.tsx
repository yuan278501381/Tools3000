/* @vitest-environment jsdom */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, render, screen, fireEvent, waitFor } from '@testing-library/react';
import { WhatsNewModal } from './WhatsNewModal';
import { compareSemver, formatLocalDateTime } from './whatsNewUtils';
import changelogData from '../generated/changelogData.json';

const bridgeRequestMock = vi.fn();
const openExternalUrlMock = vi.fn();

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, params?: Record<string, unknown>) => {
      if (params && Object.keys(params).length > 0) {
        return `${key}:${JSON.stringify(params)}`;
      }
      return key;
    },
    i18n: { language: 'zh-CN' },
  }),
}));

vi.mock('../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
  openExternalUrl: (...args: unknown[]) => openExternalUrlMock(...args),
}));

describe('WhatsNewModal', () => {
  afterEach(() => {
    cleanup();
  });

  beforeEach(() => {
    bridgeRequestMock.mockReset();
    openExternalUrlMock.mockReset();
    bridgeRequestMock.mockResolvedValue({ success: true });
    openExternalUrlMock.mockResolvedValue(true);
  });

  it('compares semantic versions and formats local date-time deterministically', () => {
    expect(compareSemver('1.4.14', '1.4.11')).toBeGreaterThan(0);
    expect(compareSemver('1.4.2', '1.4.10')).toBeLessThan(0);
    expect(compareSemver('v1.4.0', '1.4.0')).toBe(0);
    expect(formatLocalDateTime('2026-11-26T01:18:46+08:00')).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/);
    expect(formatLocalDateTime('invalid-date')).toBe('');
  });

  it('renders manual archive mode with top 10 releases, pagination, and keyword search', async () => {
    const onClose = vi.fn();
    const onToggle = vi.fn();
    const latest = changelogData.releases[0];

    render(
      <WhatsNewModal
        open
        mode="manual"
        currentVersion={latest.version}
        showChangelogOnUpdate
        onToggleShowOnUpdate={onToggle}
        onClose={onClose}
      />,
    );

    // 等待离线懒加载分块就绪：首屏展示最新 10 个版本，且默认展开最新版本
    const loadMoreBtn = await screen.findByRole('button', { name: /changelog\.loadMoreBtn/i });
    expect(screen.getAllByText(latest.tag).length).toBeGreaterThanOrEqual(2);
    expect(screen.getAllByRole('article').length).toBe(10);

    const releaseButtons = screen.getAllByRole('button', { expanded: true });
    expect(releaseButtons.length).toBe(1);

    fireEvent.click(loadMoreBtn);

    await waitFor(() => {
      expect(screen.getAllByRole('article').length).toBeGreaterThanOrEqual(20);
    });

    // 关键词即时搜索过滤并自动展开命中版本
    const searchInput = screen.getByPlaceholderText('changelog.searchPlaceholder');
    fireEvent.change(searchInput, { target: { value: 'v1.3.0' } });

    await waitFor(() => {
      expect(screen.getAllByText('v1.3.0').length).toBeGreaterThanOrEqual(1);
      expect(screen.getAllByRole('article').length).toBeGreaterThanOrEqual(1);
    });

    // 底部自动弹窗开关双向同步至 general.updateSettings
    const checkbox = screen.getByRole('checkbox');
    fireEvent.click(checkbox);
    await waitFor(() => {
      expect(onToggle).toHaveBeenCalledWith(false);
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'general.updateSettings',
        { showChangelogOnUpdate: false },
        { silent: true },
      );
    });
  });

  it('renders upgrade mode with multi-version span banner and caps auto-expanded NEW versions to 3', async () => {
    const onClose = vi.fn();
    const latest = changelogData.releases[0];
    const fiveEarlier = changelogData.releases[5];

    render(
      <WhatsNewModal
        open
        mode="upgrade"
        currentVersion={latest.version}
        previousVersion={fiveEarlier.version}
        showChangelogOnUpdate
        onToggleShowOnUpdate={vi.fn()}
        onClose={onClose}
      />,
    );

    // 跨 5 版迁移顶部展示迁移横幅
    expect(await screen.findByText(/changelog\.upgradeSpanBanner/)).toBeTruthy();

    // 超过 3 个新版本时仅自动展开最新 3 版，防止长屏过载
    await waitFor(() => {
      const expandedBtns = screen.getAllByRole('button', { expanded: true });
      expect(expandedBtns.length).toBe(3);
    });

    // 所有大于起点版本的记录均带 NEW 徽章
    const newBadges = screen.getAllByText('changelog.badgeNew');
    expect(newBadges.length).toBe(5);

    // 按 Escape 关闭弹窗
    fireEvent.keyDown(window, { key: 'Escape' });
    expect(onClose).toHaveBeenCalledTimes(1);
  });
});

/* @vitest-environment jsdom */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, render, screen, fireEvent, waitFor } from '@testing-library/react';
import { WhatsNewModal } from './WhatsNewModal';
import {
  compareSemver,
  formatLocalDate,
  groupReleasesByLocalDate,
  parseInlineMarkdownAst,
  splitLeadInBullet,
  computeBlockCategoryCounts,
  type ChangelogBundle,
} from './whatsNewUtils';
import changelogDataRaw from '../generated/changelogData.json';

const changelogData = changelogDataRaw as unknown as ChangelogBundle;

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

  it('compares semantic versions, formats local date-time, and parses nested bold+code AST cleanly', () => {
    expect(compareSemver('1.4.14', '1.4.11')).toBeGreaterThan(0);
    expect(compareSemver('1.4.2', '1.4.10')).toBeLessThan(0);
    expect(compareSemver('v1.4.0', '1.4.0')).toBe(0);
    // 构造跨时区稳健断言（覆盖 UTC+0 CI Runner、东八区及负偏移时区）
    const sampleIso = '2026-11-26T01:18:46+08:00';
    const sampleDate = new Date(sampleIso);
    const expectedLocal = `${sampleDate.getFullYear()}-${String(sampleDate.getMonth() + 1).padStart(2, '0')}-${String(sampleDate.getDate()).padStart(2, '0')}`;
    expect(formatLocalDate(sampleIso)).toBe(expectedLocal);
    expect(formatLocalDate('2026-11-26T12:00:00Z')).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    expect(formatLocalDate('invalid-date')).toBe('');

    // 按自然日分组：同日多个版本应归入同一天且保持原顺序（不受跨日首版仅 1 条影响）
    const grouped = groupReleasesByLocalDate(changelogData.releases.slice(0, 10));
    expect(grouped.length).toBeLessThan(10);
    expect(grouped.some((g) => g.releases.length > 1)).toBe(true);

    // 防回退断言：彻底根治 `**SQL Server (`ssms.exe`)**` 中反引号裸露 Bug
    const ast = parseInlineMarkdownAst('避让 **SQL Server (`ssms.exe`)** 与 **Emacs (`emacs.exe`)**');
    expect(ast).toEqual([
      { type: 'text', value: '避让 ' },
      {
        type: 'strong',
        children: [
          { type: 'text', value: 'SQL Server (' },
          { type: 'code', value: 'ssms.exe' },
          { type: 'text', value: ')' },
        ],
      },
      { type: 'text', value: ' 与 ' },
      {
        type: 'strong',
        children: [
          { type: 'text', value: 'Emacs (' },
          { type: 'code', value: 'emacs.exe' },
          { type: 'text', value: ')' },
        ],
      },
    ]);

    expect(splitLeadInBullet('**三模式自由选择**：详细说明')).toEqual({
      lead: '三模式自由选择',
      body: '详细说明',
    });
    expect(splitLeadInBullet('无前缀说明正文')).toEqual({
      body: '无前缀说明正文',
    });
  });

  it('renders categorized counts, supports category filtering, compact/detailed view, and zero raw backticks', async () => {
    const onClose = vi.fn();
    const onToggle = vi.fn();
    const latest = changelogData.releases[0];
    const v1416 = changelogData.releases.find((r) => r.version === '1.4.16');
    expect(v1416).toBeTruthy();
    const counts1416 = computeBlockCategoryCounts(v1416!.zh);
    expect(counts1416.feature).toBeGreaterThan(0);
    expect(counts1416.fix).toBeGreaterThan(0);
    expect(counts1416.total).toBe(counts1416.feature + counts1416.improvement + counts1416.fix);

    const { container } = render(
      <WhatsNewModal
        open
        mode="manual"
        currentVersion={latest.version}
        showChangelogOnUpdate
        onToggleShowOnUpdate={onToggle}
        onClose={onClose}
      />,
    );

    // 等待离线懒加载分块就绪：首屏展示最新 10 个版本，按自然日分组且不暴露 HH:mm 时刻
    const loadMoreBtn = await screen.findByRole('button', { name: /changelog\.loadMoreBtn/i });
    expect(screen.getAllByText(latest.tag).length).toBeGreaterThanOrEqual(2);
    expect(screen.getAllByRole('article').length).toBe(10);
    expect(container.querySelectorAll('.wn-day-group').length).toBeGreaterThanOrEqual(1);
    expect(container.querySelectorAll('.wn-day-group').length).toBeLessThan(10);
    expect(container.textContent).not.toMatch(/\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}/);
    expect(screen.getAllByText(/changelog\.dayReleaseCount/).length).toBeGreaterThanOrEqual(1);

    // 展开 v1.4.16 验证：无任何裸露反引号，且 GitHub Issue 徽章可直链打开
    const v1416Header = screen.getByRole('button', { name: /v1\.4\.16/i });
    fireEvent.click(v1416Header);

    await waitFor(() => {
      expect(container.textContent).not.toContain('`ssms.exe`');
      expect(container.textContent).toContain('ssms.exe');
    });

    const issueChip = screen.getByRole('button', { name: /#34/i });
    fireEvent.click(issueChip);
    expect(openExternalUrlMock).toHaveBeenCalledWith('https://github.com/yuan278501381/Tools3000/issues/34');

    // 切换至「简览」模式应折叠子要点列表
    const compactBtn = screen.getByRole('button', { name: 'changelog.densityCompact' });
    fireEvent.click(compactBtn);
    await waitFor(() => {
      expect(container.querySelectorAll('.wn-entry__list').length).toBe(0);
    });

    // 切换至「仅看新功能」筛选：v1.4.17 仅有修复应被滤除，v1.4.16 保留且仅呈现 feature 组
    const featureTab = screen.getByRole('tab', { name: /changelog\.categoryFeature/i });
    fireEvent.click(featureTab);
    await waitFor(() => {
      expect(container.querySelectorAll('.wn-group--fix').length).toBe(0);
    });

    // 重置回全部并测试分页与关键词搜索高亮
    fireEvent.click(screen.getByRole('tab', { name: /changelog\.filterAll/i }));
    fireEvent.click(loadMoreBtn);
    await waitFor(() => {
      expect(screen.getAllByRole('article').length).toBeGreaterThanOrEqual(20);
    });

    const searchInput = screen.getByPlaceholderText('changelog.searchPlaceholder');
    const allTabBefore = screen.getByRole('tab', { name: /changelog\.filterAll/i }).textContent ?? '';
    fireEvent.change(searchInput, { target: { value: 'v1.3.0' } });
    await waitFor(() => {
      expect(screen.getAllByText('v1.3.0').length).toBeGreaterThanOrEqual(1);
    });
    // 分面计数应随搜索词动态收敛而非停留在全局总数
    const allTabAfter = screen.getByRole('tab', { name: /changelog\.filterAll/i }).textContent ?? '';
    expect(allTabAfter).not.toBe(allTabBefore);

    // 搜索态首次按 Escape 应优先清空搜索词而非直接关闭弹窗
    fireEvent.keyDown(window, { key: 'Escape' });
    await waitFor(() => {
      expect((searchInput as HTMLInputElement).value).toBe('');
      expect(onClose).not.toHaveBeenCalled();
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

    expect(await screen.findByText(/changelog\.upgradeSpanBanner/)).toBeTruthy();

    await waitFor(() => {
      const releaseHeaders = screen
        .getAllByRole('button', { expanded: true })
        .filter((el) => el.classList.contains('wn-release__header'));
      expect(releaseHeaders.length).toBe(3);
    });

    const newBadges = screen.getAllByText('changelog.badgeNew');
    expect(newBadges.length).toBe(5);

    fireEvent.keyDown(window, { key: 'Escape' });
    expect(onClose).toHaveBeenCalledTimes(1);
  });
});

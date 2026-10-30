/* ─────────────────────────────────────────────────────────────────────────────
 * SearchPage.tsx — 文件搜索配置与语法手册
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Button, Tabs, type TabItem, CodeBadge } from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import {
  Search,
  HardDrive,
  Code2,
  BookOpen,
  CheckCircle2,
  FolderSearch,
  Sparkles,
  HelpCircle,
  Play,
  RotateCw
} from 'lucide-react';
import './SearchPage.css';

interface SearchSettings {
  hotkey: string;
  maxResults: number;
  defaultCategory: string;
  caseSensitive: boolean;
  matchPath: boolean;
  pinyinEnabled: boolean;
  iconStyle?: 'native' | 'vector';
  autoBypassFullscreen?: boolean;
}

interface ServiceStatus {
  available: boolean;
  pipeName: string;
}

export const SearchPage: FC = () => {
  const { t } = useTranslation();
  const [activeTab, setActiveTab] = useState<'settings' | 'syntax' | 'regex' | 'status'>('settings');
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [settings, setSettings] = useState<SearchSettings>({
    hotkey: 'Alt+Space',
    maxResults: 50,
    defaultCategory: 'all',
    caseSensitive: false,
    matchPath: false,
    pinyinEnabled: true,
    iconStyle: 'native',
    autoBypassFullscreen: true,
  });
  const [serviceStatus, setServiceStatus] = useState<ServiceStatus>({
    available: false,
    pipeName: '\\\\.\\pipe\\Tools3000SearchPipe',
  });
  const [checking, setChecking] = useState(false);

  const getHotkey = (name: string) => hotkeys.find(h => h.name === name);

  const loadData = () => {
    bridgeRequest<SearchSettings>('search.getSettings')
      .then(res => setSettings(prev => ({ ...prev, ...res })))
      .catch(console.error);

    bridgeRequest<HotkeyEntry[]>('hotkey.getAll')
      .then(res => setHotkeys(Array.isArray(res) ? res : []))
      .catch(console.error);

    bridgeRequest<ServiceStatus>('search.getServiceStatus')
      .then(res => {
        if (res && typeof res === 'object') {
          setServiceStatus(prev => ({ ...prev, ...res }));
        }
      })
      .catch(console.error);
  };

  useEffect(() => {
    loadData();
  }, []);

  const saveSetting = async <K extends keyof SearchSettings>(key: K, value: SearchSettings[K]) => {
    setSettings(prev => ({ ...prev, [key]: value }));
    try {
      await bridgeRequest('search.saveSettings', { [key]: value });
    } catch {
      void bridgeRequest<SearchSettings>('search.getSettings')
        .then(res => setSettings(prev => ({ ...prev, ...res })))
        .catch(() => {});
    }
  };

  const handleHotkeyChange = async (newKey: string) => {
    try {
      const res = await bridgeRequest<{ success: boolean; error?: string; shortcut?: string }>('hotkey.rebind', {
        name: 'Toggle Search',
        hotkey: newKey,
      });
      if (res.success) {
        await saveSetting('hotkey', res.shortcut ?? newKey);
        const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
        if (Array.isArray(refreshed)) setHotkeys(refreshed);
      }
    } catch {
      // 错误已由 bridgeRequest 自动提示失败
    }
  };

  const checkService = async () => {
    setChecking(true);
    try {
      const res = await bridgeRequest<ServiceStatus>('search.getServiceStatus');
      if (res && typeof res === 'object') {
        setServiceStatus(prev => ({ ...prev, ...res }));
      }
      if (res?.available) {
        toast.success(t('searchPage.serviceOk', 'Index daemon connected via local named pipe'));
      } else {
        toast.error(t('searchPage.serviceDown', 'Index service not responding, check administrator privileges'));
      }
    } catch {
      toast.error(t('searchPage.serviceDown', 'Index service not responding, check administrator privileges'));
    } finally {
      setChecking(false);
    }
  };

  const launchSearch = () => {
    void bridgeRequest('search.toggle').catch(console.error);
  };

  const tabs: TabItem<'settings' | 'syntax' | 'regex' | 'status'>[] = [
    { id: 'settings', label: t('searchPage.tabSettings', 'Settings & Hotkey'), icon: <Search size={16} /> },
    { id: 'syntax', label: t('searchPage.tabSyntax', 'Search Syntax Manual'), icon: <BookOpen size={16} /> },
    { id: 'regex', label: t('searchPage.tabRegex', 'Regular Expressions'), icon: <Code2 size={16} /> },
    { id: 'status', label: t('searchPage.tabStatus', 'Service & Index'), icon: <HardDrive size={16} /> },
  ];

  return (
    <div className="search-page">
      {/* ── 顶部选项卡 ─────────────────────────────────────────────── */}
      <Tabs
        tabs={tabs}
        activeId={activeTab}
        onChange={(id) => setActiveTab(id as typeof activeTab)}
        ariaLabel={t('search.title', 'Quick file search')}
      />

      {/* ── 1. 基础设置 ────────────────────────────────────────────── */}
      {activeTab === 'settings' && (
        <div className="search-page__content">
          <SettingGroup title={t('searchPage.hotkeyConfig', 'Hotkey & Trigger')} icon={<Sparkles size={18} />}>
            <Card>
              <SettingRow
                label={
                  <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                    <span>{t('searchPage.toggleHotkey', 'Toggle Search Bar Shortcut')}</span>
                    <HotkeyStatusBadge entry={getHotkey('Toggle Search')} />
                  </div>
                }
                description={t('searchPage.toggleHotkeyDesc', 'Default Alt + Space, instantly summons the Fluent search overlay')}
              >
                <HotkeyRecorder
                  id="search-toggle-hotkey"
                  value={settings.hotkey}
                  onChange={handleHotkeyChange}
                  placeholder="Alt+Space"
                />
              </SettingRow>

              <SettingRow
                label={t('searchPage.autoBypassFullscreen', 'Fullscreen Game/Video Bypass')}
                description={t('searchPage.autoBypassFullscreenDesc', 'Automatically suppress search hotkey when foreground window is in exclusive fullscreen mode to prevent disrupting gaming or video playback')}
              >
                <Toggle
                  id="search-auto-bypass-toggle"
                  checked={settings.autoBypassFullscreen ?? true}
                  onChange={v => saveSetting('autoBypassFullscreen', v)}
                />
              </SettingRow>

              <SettingRow
                label={t('searchPage.quickTest', 'Test Search Bar')}
                description={t('searchPage.quickTestDesc', 'Test the floating search overlay animation and latency')}
              >
                <Button variant="primary" onClick={launchSearch}>
                  <Play size={14} style={{ marginRight: 6 }} />
                  {t('searchPage.openNow', 'Summon Search Bar')}
                </Button>
              </SettingRow>
            </Card>
          </SettingGroup>

          <SettingGroup title={t('searchPage.searchBehavior', 'Preferences & Behavior')} icon={<FolderSearch size={18} />}>
            <Card>
              <SettingRow
                label={t('searchPage.pinyinTitle', 'Smart Pinyin Search')}
                description={t('searchPage.pinyinDesc', 'Supports Chinese Pinyin initials (e.g. wx for WeChat) and full pinyin')}
              >
                <Toggle
                  id="search-pinyin-toggle"
                  checked={settings.pinyinEnabled}
                  onChange={v => saveSetting('pinyinEnabled', v)}
                />
              </SettingRow>

              <SettingRow
                label={t('searchPage.matchPathTitle', 'Match Full Path by Default')}
                description={t('searchPage.matchPathDesc', 'Search within file paths in addition to file names')}
              >
                <Toggle
                  id="search-matchpath-toggle"
                  checked={settings.matchPath}
                  onChange={v => saveSetting('matchPath', v)}
                />
              </SettingRow>

              <SettingRow
                label={t('searchPage.caseTitle', 'Case Sensitive')}
                description={t('searchPage.caseDesc', 'Match exact case; can also be toggled with case: prefix in query')}
              >
                <Toggle
                  id="search-case-toggle"
                  checked={settings.caseSensitive}
                  onChange={v => saveSetting('caseSensitive', v)}
                />
              </SettingRow>

              <SettingRow
                label={t('searchPage.iconStyleTitle', 'Result Icon Style')}
                description={t('searchPage.iconStyleDesc', 'Choose between Windows native associated file icons (familiar system look with 0 cognitive overhead) and minimalist vector icons')}
              >
                <div style={{ display: 'flex', gap: '6px' }}>
                  <button
                    type="button"
                    className={`popover-segment ${(settings.iconStyle ?? 'native') === 'native' ? 'popover-segment--active' : ''}`}
                    onClick={() => void saveSetting('iconStyle', 'native')}
                    style={{
                      padding: '4px 10px',
                      borderRadius: '6px',
                      border: `1px solid ${(settings.iconStyle ?? 'native') === 'native' ? 'var(--primary)' : 'var(--border)'}`,
                      background: (settings.iconStyle ?? 'native') === 'native' ? 'var(--primary)' : 'var(--bg-elevated)',
                      color: (settings.iconStyle ?? 'native') === 'native' ? '#ffffff' : 'var(--text-secondary)',
                      fontSize: '12.5px',
                      fontWeight: 600,
                      cursor: 'pointer'
                    }}
                  >
                    {t('searchPage.iconStyleNative', 'Windows Native')}
                  </button>
                  <button
                    type="button"
                    className={`popover-segment ${settings.iconStyle === 'vector' ? 'popover-segment--active' : ''}`}
                    onClick={() => void saveSetting('iconStyle', 'vector')}
                    style={{
                      padding: '4px 10px',
                      borderRadius: '6px',
                      border: `1px solid ${settings.iconStyle === 'vector' ? 'var(--primary)' : 'var(--border)'}`,
                      background: settings.iconStyle === 'vector' ? 'var(--primary)' : 'var(--bg-elevated)',
                      color: settings.iconStyle === 'vector' ? '#ffffff' : 'var(--text-secondary)',
                      fontSize: '12.5px',
                      fontWeight: 600,
                      cursor: 'pointer'
                    }}
                  >
                    {t('searchPage.iconStyleVector', 'Vector Minimalist')}
                  </button>
                </div>
              </SettingRow>

            </Card>
          </SettingGroup>
        </div>
      )}

      {/* ── 2. 搜索语法手册 ────────────────────────────────────────── */}
      {activeTab === 'syntax' && (
        <div className="search-page__content">
          <Card title={t('searchPage.operatorsTitle', 'Operators & Combination')}>
            <div className="syntax-table-wrapper">
              <table className="syntax-table">
                <thead>
                  <tr>
                    <th>{t('searchPage.thOpSyntax', 'Operator / Syntax')}</th>
                    <th>{t('searchPage.thDesc', 'Description')}</th>
                    <th>{t('searchPage.thExample', 'Example')}</th>
                  </tr>
                </thead>
                <tbody>
                  <tr>
                    <td><CodeBadge>space</CodeBadge></td>
                    <td>{t('searchPage.opSpaceDesc', 'Logical AND, matches all keywords simultaneously')}</td>
                    <td><CodeBadge>report 2026</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>|</CodeBadge> / <CodeBadge>OR</CodeBadge></td>
                    <td>{t('searchPage.opOrDesc', 'Logical OR, matches any condition')}</td>
                    <td><CodeBadge>ext:jpg | ext:png</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>!</CodeBadge> / <CodeBadge>NOT</CodeBadge></td>
                    <td>{t('searchPage.opNotDesc', 'Logical NOT, excludes files containing specific word')}</td>
                    <td><CodeBadge>*.cpp !test</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>" "</CodeBadge></td>
                    <td>{t('searchPage.opPhraseDesc', 'Quoted phrase, strictly matches exact text with spaces')}</td>
                    <td><CodeBadge>"Program Files"</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>*</CodeBadge></td>
                    <td>{t('searchPage.opWildcardMulti', 'Wildcard, matches 0 or more characters')}</td>
                    <td><CodeBadge>*.pdf</CodeBadge>, <CodeBadge>report_*</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>?</CodeBadge></td>
                    <td>{t('searchPage.opWildcardSingle', 'Wildcard, matches 1 character')}</td>
                    <td><CodeBadge>img_??.png</CodeBadge></td>
                  </tr>
                </tbody>
              </table>
            </div>
          </Card>

          <Card title={t('searchPage.modifiersTitle', 'Filters & Modifiers')}>
            <div className="syntax-table-wrapper">
              <table className="syntax-table">
                <thead>
                  <tr>
                    <th>{t('searchPage.thModifier', 'Prefix / Modifier')}</th>
                    <th>{t('searchPage.thDesc', 'Description')}</th>
                    <th>{t('searchPage.thExample', 'Example')}</th>
                  </tr>
                </thead>
                <tbody>
                  <tr>
                    <td><CodeBadge>ext:&lt;ext_list&gt;</CodeBadge></td>
                    <td>{t('searchPage.modExtDesc', 'Filter by file extensions, comma or semicolon separated')}</td>
                    <td><CodeBadge>ext:jpg;png;webp</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>file:</CodeBadge></td>
                    <td>{t('searchPage.modFileDesc', 'Match files only (exclude folders)')}</td>
                    <td><CodeBadge>file: *.txt</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>folder:</CodeBadge> / <CodeBadge>dir:</CodeBadge></td>
                    <td>{t('searchPage.modFolderDesc', 'Match folders / directories only')}</td>
                    <td><CodeBadge>folder: project</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>path:&lt;path&gt;</CodeBadge></td>
                    <td>{t('searchPage.modPathDesc', 'Search within full absolute path')}</td>
                    <td><CodeBadge>path:windows\system32</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>parent:&lt;dir&gt;</CodeBadge> / <CodeBadge>p:</CodeBadge></td>
                    <td>{t('searchPage.modParentDesc', 'Match in direct parent directory name')}</td>
                    <td><CodeBadge>parent:tools3000</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>c:</CodeBadge> / <CodeBadge>d:</CodeBadge></td>
                    <td>{t('searchPage.modDriveDesc', 'Restrict search to specified drive')}</td>
                    <td><CodeBadge>d: *.zip</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>exact:&lt;name&gt;</CodeBadge></td>
                    <td>{t('searchPage.modExactDesc', 'Strict exact filename match')}</td>
                    <td><CodeBadge>exact:README.md</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>content:&lt;query&gt;</CodeBadge></td>
                    <td>{t('searchPage.modContentDesc', 'Full-text content search, supports code, Office documents, PSD/AI/CAD drawings')}</td>
                    <td><CodeBadge>content:SELECT</CodeBadge>, <CodeBadge>ext:docx content:contract</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>case:&lt;text&gt;</CodeBadge> / <CodeBadge>cs:</CodeBadge></td>
                    <td>{t('searchPage.modCaseDesc', 'Enforce case-sensitive matching')}</td>
                    <td><CodeBadge>case:Tools3000</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>pinyin:&lt;pinyin&gt;</CodeBadge> / <CodeBadge>py:</CodeBadge></td>
                    <td>{t('searchPage.modPinyinDesc', 'Explicit pinyin search (full spell or initials)')}</td>
                    <td><CodeBadge>py:wx</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>nopy:&lt;text&gt;</CodeBadge></td>
                    <td>{t('searchPage.modNopyDesc', 'Disable pinyin, match plain text only')}</td>
                    <td><CodeBadge>nopy:wx</CodeBadge></td>
                  </tr>
                </tbody>
              </table>
            </div>
          </Card>
        </div>
      )}

      {/* ── 3. 正则表达式手册 ──────────────────────────────────────── */}
      {activeTab === 'regex' && (
        <div className="search-page__content">
          <Card title={t('searchPage.regexTitle', 'Regular Expression Query (regex: or r:)')}>
            <p className="syntax-hint-p">
              {t('searchPage.regexHint', 'Use regex:<expr> or r:<expr> prefix to enable standard C++ ECMAScript regular expression search.')}
            </p>
            <div className="syntax-table-wrapper">
              <table className="syntax-table">
                <thead>
                  <tr>
                    <th>{t('searchPage.thRegexSyntax', 'Regex Syntax')}</th>
                    <th>{t('searchPage.thRegexRule', 'Matching Rule')}</th>
                    <th>{t('searchPage.thRegexExample', 'Practical Example')}</th>
                  </tr>
                </thead>
                <tbody>
                  <tr>
                    <td><CodeBadge>a|b</CodeBadge></td>
                    <td>{t('searchPage.regOr', 'Match expression a or b')}</td>
                    <td><CodeBadge>r:test|debug</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>.</CodeBadge></td>
                    <td>{t('searchPage.regAnyChar', 'Match any single character except newline')}</td>
                    <td><CodeBadge>r:a.c\.txt</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>[abc]</CodeBadge></td>
                    <td>{t('searchPage.regCharSet', 'Match any single character in character set')}</td>
                    <td><CodeBadge>r:log_[0-9]\.txt</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>[^abc]</CodeBadge></td>
                    <td>{t('searchPage.regNegSet', 'Exclude characters in character set')}</td>
                    <td><CodeBadge>r:file_[^0-9]\.dat</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>[a-z]</CodeBadge> / <CodeBadge>[0-9]</CodeBadge></td>
                    <td>{t('searchPage.regRange', 'Match characters within specified range')}</td>
                    <td><CodeBadge>{'r:^[a-z]{3}_[0-9]{4}'}</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>^</CodeBadge></td>
                    <td>{t('searchPage.regStartAnchor', 'Match start of filename (head anchor)')}</td>
                    <td><CodeBadge>r:^Tools3000.*\.exe$</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>$</CodeBadge></td>
                    <td>{t('searchPage.regEndAnchor', 'Match end of filename (tail anchor)')}</td>
                    <td><CodeBadge>r:\.min\.js$</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>*</CodeBadge> / <CodeBadge>+</CodeBadge> / <CodeBadge>?</CodeBadge></td>
                    <td>{t('searchPage.regQuantifiers', 'Match 0 or more / 1 or more / 0 or 1 time')}</td>
                    <td><CodeBadge>r:v\d+\.\d+</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>&#123;n&#125;</CodeBadge> / <CodeBadge>&#123;n,m&#125;</CodeBadge></td>
                    <td>{t('searchPage.regExactCount', 'Specify exact match count or range')}</td>
                    <td><CodeBadge>r:\d&#123;4&#125;-\d&#123;2&#125;-\d&#123;2&#125;</CodeBadge></td>
                  </tr>
                  <tr>
                    <td><CodeBadge>\</CodeBadge></td>
                    <td>{t('searchPage.regEscape', 'Escape special characters (e.g. \\. matches literal dot)')}</td>
                    <td><CodeBadge>r:archive\.(tar\.gz|zip)</CodeBadge></td>
                  </tr>
                </tbody>
              </table>
            </div>
          </Card>
        </div>
      )}

      {/* ── 4. 磁盘与服务状态 ──────────────────────────────────────── */}
      {activeTab === 'status' && (
        <div className="search-page__content">
          <Card title={t('searchPage.serviceMonitor', 'NTFS MFT Engine Monitor')}>
            <div className="service-status-card">
              <div className="service-status-header">
                <div className={`status-indicator ${serviceStatus.available ? 'status-indicator--online' : 'status-indicator--offline'}`}>
                  {serviceStatus.available ? <CheckCircle2 size={24} /> : <HelpCircle size={24} />}
                </div>
                <div className="status-info">
                  <h3 className="status-title">
                    {serviceStatus.available ? t('searchPage.serviceRunning', 'Indexing Engine Running (Local Named Pipe)') : t('searchPage.serviceStopped', 'Indexing Engine Disconnected')}
                  </h3>
                  <p className="status-desc">
                    {serviceStatus.available
                      ? t('searchPage.serviceRunningDesc', 'NTFS MFT tree and USN journal monitoring are mounted and ready')
                      : t('searchPage.serviceStoppedDesc', 'Attempting background auto-start; or click below to retry')}
                  </p>
                </div>
                <Button variant="ghost" onClick={checkService} disabled={checking}>
                  <RotateCw size={14} className={checking ? 'animate-spin' : ''} style={{ marginRight: 6 }} />
                  {t('searchPage.checkNow', 'Check Now')}
                </Button>
              </div>

              <div className="service-details-grid">
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.pipeAddr', 'IPC Pipe Address')}</span>
                  <span className="detail-value"><CodeBadge>{serviceStatus.pipeName}</CodeBadge></span>
                </div>
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.indexTech', 'Indexing Architecture')}</span>
                  <span className="detail-value">{t('searchPage.indexTechDesc', 'NTFS MFT memory tree + USN Journal incremental monitoring')}</span>
                </div>
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.pinyinTech', 'Pinyin Engine')}</span>
                  <span className="detail-value">{t('searchPage.pinyinTechDesc', 'Zero-heap allocation GBK/Unicode bi-directional pinyin table')}</span>
                </div>
                <div className="detail-item">
                  <span className="detail-label">{t('searchPage.daemonMode', 'Daemon Mode')}</span>
                  <span className="detail-value">{t('searchPage.daemonModeDesc', 'Dual-mode adaptive (Windows Service / Standalone Windowless Daemon)')}</span>
                </div>
              </div>
            </div>
          </Card>
        </div>
      )}
    </div>
  );
};

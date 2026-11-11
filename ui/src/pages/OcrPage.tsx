/* ─────────────────────────────────────────────────────────────────────────────
/* ─────────────────────────────────────────────────────────────────────────────
 * OcrPage — OCR 识别设置页
 *
 * 引擎: Windows.Media.Ocr (系统内置、离线)。
 * 从 C++ ConfigManager 加载设置，修改后实时 IPC 保存；并展示引擎可用性。
 * ───────────────────────────────────────────────────────────────────────────── */

import { useState, useEffect, useCallback, type FC } from 'react';
import { Card, Toggle, SettingRow, SettingGroup, Badge, Select, PageSkeleton } from '../components/UIKit';
import { HotkeyRecorder } from '../components/HotkeyRecorder';
import { HotkeyStatusBadge, type HotkeyEntry } from '../components/HotkeyStatusBadge';
import { bridgeRequest } from '../hooks/useBridge';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { FileText } from 'lucide-react';

interface OcrSettings {
  engine: string;
  language: string;
  copyResult: boolean;
  showResultWindow: boolean;
}

interface OperationResult {
  success: boolean;
  error?: string;
  shortcut?: string;
  conflictType?: string;
  conflictWith?: string;
}

export const OcrPage: FC = () => {
  const [settings, setSettings] = useState<OcrSettings>({
    engine: 'windows',
    language: 'auto',
    copyResult: true,
    showResultWindow: true,
  });
  const [loading, setLoading] = useState(true);
  const [available, setAvailable] = useState(false);
  const [hotkeys, setHotkeys] = useState<HotkeyEntry[]>([]);
  const [shortcut, setShortcut] = useState('Ctrl+Shift+O');
  const { t } = useTranslation();

  const getHotkey = (name: string) => hotkeys.find(h => h.name === name);

  useEffect(() => {
    Promise.all([
      bridgeRequest<OcrSettings>('ocr.getSettings'),
      bridgeRequest<{ available: boolean }>('ocr.getStatus'),
      bridgeRequest<HotkeyEntry[]>('hotkey.getAll'),
      bridgeRequest<string>('config.get', { key: '/ocr/language' }, { silent: true }).catch(() => null),
    ]).then(([data, status, hotkeyList, cfgLang]) => {
      const resolvedLanguage =
        (typeof data?.language === 'string' && data.language.trim() !== '' ? data.language : null) ??
        (typeof cfgLang === 'string' && cfgLang.trim() !== '' ? cfgLang : 'auto');
      setSettings(prev => ({ ...prev, ...data, language: resolvedLanguage }));
      setAvailable(status.available);
      const list = Array.isArray(hotkeyList) ? hotkeyList : [];
      setHotkeys(list);
      setShortcut(list.find(item => item.name === 'OCR')?.shortcut ?? 'Ctrl+Shift+O');
    }).catch((error) => {
      console.error(error);
      toast.error(t('ocr.loadFailed'));
    }).finally(() => setLoading(false));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const updateSetting = useCallback((key: keyof OcrSettings, value: unknown) => {
    setSettings(prev => ({ ...prev, [key]: value }));
    if (key === 'language') {
      void (async () => {
        try {
          const direct = await bridgeRequest<OperationResult>('ocr.updateSettings', { language: value }, { silent: true }).catch(() => null);
          if (!direct?.success) {
            await bridgeRequest('config.set', { key: '/ocr/language', value });
          }
        } catch (error) {
          toast.error(t('ocr.saveFailed'), { description: String(error) });
        }
      })();
      return;
    }
    bridgeRequest<OperationResult>('ocr.updateSettings', { [key]: value })
      .then(result => { if (!result.success) throw new Error(result.error || 'update failed'); })
      .catch(async (error) => {
        toast.error(t('ocr.saveFailed'), { description: String(error) });
        try { setSettings(await bridgeRequest<OcrSettings>('ocr.getSettings')); } catch { /* keep usable */ }
      });
  }, [t]);

  const rebindShortcut = async (value: string) => {
    try {
      const result = await bridgeRequest<OperationResult>('hotkey.rebind', { name: 'OCR', hotkey: value });
      if (!result.success) throw new Error(result.error || t('hotkey.bindFailed'));
      setShortcut(result.shortcut ?? value);
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    } catch (error) {
      toast.error(t('hotkey.bindFailed'), { description: String(error) });
      const refreshed = await bridgeRequest<HotkeyEntry[]>('hotkey.getAll');
      if (Array.isArray(refreshed)) setHotkeys(refreshed);
    }
  };

  if (loading) {
    return <PageSkeleton cards={1} rowsPerCard={4} />;
  }

  return (
    <div className="ocr-page" style={{ animation: 'fadeIn 0.3s ease' }}>
      <SettingGroup title={t('ocr.title')} icon={<FileText size={20} strokeWidth={2.5} />}>
        <Card>
          <SettingRow
            label={
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                <span>{t('ocr.shortcut')}</span>
                <HotkeyStatusBadge entry={getHotkey('OCR')} />
              </div>
            }
            description={t('ocr.shortcutDesc')}
          >
            <HotkeyRecorder
              id="ocr-shortcut"
              value={shortcut}
              onChange={(v) => void rebindShortcut(v)}
            />
          </SettingRow>
          
          <SettingRow label={t('ocr.engine')} description={t('ocr.engineDesc')}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <span>{t('ocr.engineWindowsOCR')}</span>
              <Badge
                text={available ? t('ocr.available') : t('ocr.unavailable')}
                variant={available ? 'success' : 'muted'}
              />
            </div>
          </SettingRow>

          <SettingRow label={t('ocr.language')} description={t('ocr.languageDesc')}>
            <Select
              id="ocr-language"
              value={settings.language || 'auto'}
              onChange={(val) => updateSetting('language', val)}
              options={[
                { value: 'auto', label: t('ocr.langAuto') },
                { value: 'zh-Hans-CN', label: t('ocr.langZhHans') },
                { value: 'zh-Hant-TW', label: t('ocr.langZhHant') },
                { value: 'en-US', label: t('ocr.langEnUs') },
                { value: 'ja-JP', label: t('ocr.langJaJp') },
                { value: 'ko-KR', label: t('ocr.langKoKr') },
              ]}
            />
          </SettingRow>

          <Toggle
            id="copyResult"
            label={t('ocr.copyToClipboard')}
            description={t('ocr.copyToClipboardDesc')}
            checked={settings.copyResult}
            onChange={(v) => updateSetting('copyResult', v)}
          />

          <Toggle
            id="showResultWindow"
            label={t('ocr.showResultWindow')}
            description={t('ocr.showResultWindowDesc')}
            checked={settings.showResultWindow}
            onChange={(v) => updateSetting('showResultWindow', v)}
          />
        </Card>
      </SettingGroup>
    </div>
  );
};

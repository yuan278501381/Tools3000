/* @vitest-environment jsdom */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { CloudBackupCard } from './CloudBackupCard';

const bridgeRequestMock = vi.fn();

vi.mock('../../hooks/useBridge', () => ({
  bridgeRequest: (...args: unknown[]) => bridgeRequestMock(...args),
}));

vi.mock('sonner', () => ({
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    info: vi.fn(),
  },
}));

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string, opts?: string | { defaultValue?: string }) => {
      if (typeof opts === 'string') return opts;
      if (opts && typeof opts === 'object' && opts.defaultValue) return opts.defaultValue;
      return key;
    },
  }),
}));

describe('CloudBackupCard', () => {
  afterEach(() => cleanup());

  beforeEach(() => {
    bridgeRequestMock.mockReset();
    bridgeRequestMock.mockImplementation(async (method: string) => {
      if (method === 'backup.getSettings') {
        return {
          success: true,
          settings: {
            enabled: false,
            provider: 'folder',
            autoBackupOnExit: false,
            keepCount: 10,
            encryptionEnabled: false,
            folderPath: '',
            webdavUrl: '',
            webdavUsername: '',
            hasWebdavPassword: false,
            s3Endpoint: '',
            s3Region: 'auto',
            s3Bucket: '',
            s3Prefix: 'tools3000-backups/',
            s3AccessKeyId: '',
            s3ForcePathStyle: true,
            hasS3SecretKey: false,
            hasEncryptionPassphrase: false,
            lastBackupAtIso: '',
            lastBackupStatus: '',
            lastBackupFile: '',
          },
          detectedFolders: [
            {
              providerId: 'onedrive',
              label: 'Microsoft OneDrive',
              path: 'C:\\Users\\Demo\\OneDrive\\Tools3000Backups',
            },
          ],
        };
      }
      if (method === 'backup.saveSettings') {
        return {
          success: true,
          settings: {
            enabled: true,
            provider: 'folder',
            autoBackupOnExit: false,
            keepCount: 10,
            encryptionEnabled: false,
            folderPath: 'C:\\Users\\Demo\\OneDrive\\Tools3000Backups',
            webdavUrl: '',
            webdavUsername: '',
            hasWebdavPassword: false,
            s3Endpoint: '',
            s3Region: 'auto',
            s3Bucket: '',
            s3Prefix: 'tools3000-backups/',
            s3AccessKeyId: '',
            s3ForcePathStyle: true,
            hasS3SecretKey: false,
            hasEncryptionPassphrase: false,
            lastBackupAtIso: '',
            lastBackupStatus: '',
            lastBackupFile: '',
          },
        };
      }
      if (method === 'backup.testConnection') {
        return { success: true, message: 'OK' };
      }
      if (method === 'backup.backupNow') {
        return {
          success: true,
          snapshotName: 'tools3000-backup-20261124-110000.t3kbak',
          bytesTransferred: 2048,
        };
      }
      if (method === 'backup.listSnapshots') {
        return {
          success: true,
          snapshots: [
            {
              id: 'tools3000-backup-20261124-110000.t3kbak',
              fileName: 'tools3000-backup-20261124-110000.t3kbak',
              sizeBytes: 2048,
              modifiedAtIso: '2026-11-24T03:00:00Z',
              encrypted: true,
            },
          ],
        };
      }
      if (method === 'backup.restoreSnapshot') {
        return {
          success: true,
          snapshotName: 'tools3000-backup-20261124-110000.t3kbak',
          preRestoreBackupPath: 'C:\\Users\\Demo\\config.pre-restore.json',
        };
      }
      return { success: true };
    });
  });

  it('auto-fills detected OneDrive path, switches providers, backs up, and restores snapshot', async () => {
    const onRestored = vi.fn();
    render(<CloudBackupCard onConfigRestored={onRestored} />);

    await waitFor(() => {
      expect(screen.getByText('Microsoft OneDrive')).toBeTruthy();
    });

    // 1. 一键填充探测到的 OneDrive 目录
    fireEvent.click(screen.getByText('Microsoft OneDrive'));
    const folderInput = screen.getByLabelText('Backup Sync Directory') as HTMLInputElement;
    expect(folderInput.value).toBe('C:\\Users\\Demo\\OneDrive\\Tools3000Backups');

    // 2. 测试连接
    fireEvent.click(screen.getByText('Test Connection'));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith(
        'backup.testConnection',
        expect.objectContaining({
          provider: 'folder',
          folderPath: 'C:\\Users\\Demo\\OneDrive\\Tools3000Backups',
        }),
      );
    });

    // 3. 立即备份并加载云端快照列表
    fireEvent.click(screen.getByText('Backup Now'));
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('backup.backupNow');
      expect(screen.getByText('tools3000-backup-20261124-110000.t3kbak')).toBeTruthy();
      expect(screen.getByText('AES-256')).toBeTruthy();
    });

    // 4. 触发二次确认并恢复配置
    fireEvent.click(screen.getByText('Restore'));
    await waitFor(() => {
      expect(screen.getByText('Restore Configuration Snapshot')).toBeTruthy();
    });
    const restoreButtons = screen.getAllByText('Restore');
    fireEvent.click(restoreButtons[restoreButtons.length - 1]);
    await waitFor(() => {
      expect(bridgeRequestMock).toHaveBeenCalledWith('backup.restoreSnapshot', {
        snapshotId: 'tools3000-backup-20261124-110000.t3kbak',
        passphrase: undefined,
      });
      expect(onRestored).toHaveBeenCalledTimes(1);
    });

    // 5. 切换 WebDAV 与 S3 表单
    fireEvent.click(screen.getByRole('tab', { name: 'WebDAV' }));
    expect(screen.getByLabelText('WebDAV Directory URL')).toBeTruthy();
    fireEvent.click(screen.getByRole('tab', { name: 'S3 Compatible (R2 / OSS / MinIO)' }));
    expect(screen.getByLabelText('Endpoint URL')).toBeTruthy();
    expect(screen.getByLabelText('Bucket Name')).toBeTruthy();
  });
});

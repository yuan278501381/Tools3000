import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  Folder,
  FolderOpen,
  FolderX,
  HardDrive,
  FileCode,
  Pencil,
  Sparkles,
  ShieldAlert,
  Copy,
  FileText,
  AppWindow,
  Info,
  Eye,
  ClipboardCopy,
  Unlock,
} from 'lucide-react';
import { bridgeRequest } from '../hooks/useBridge';
import { IMAGE_EXTENSIONS } from './searchConstants';
import { extractParentDirectoryPath } from './searchUtils';
import type { SearchResult } from './searchTypes';

export interface SearchContextMenuProps {
  contextMenu: {
    visible: boolean;
    x: number;
    y: number;
    result?: SearchResult;
  };
  onClose: () => void;
  onOpen: (result: SearchResult) => void;
  onOpenFolder: (result: SearchResult) => void;
  onEditWithNotepad: (result: SearchResult) => void;
  onStartRename: (result: SearchResult) => void;
  onPinResult: (result: SearchResult) => void;
  onRunAsAdmin: (result: SearchResult) => void;
  onCopyPath: (result: SearchResult) => void;
  onCopyFile?: (result: SearchResult) => void;
  onQuickLook?: (result: SearchResult) => void;
  onExcludeFolder?: (result: SearchResult) => void;
  onCopyText: (text: string) => void;
  onShowProperties: (result: SearchResult) => void;
}

export const SearchContextMenu: React.FC<SearchContextMenuProps> = ({
  contextMenu,
  onClose,
  onOpen,
  onOpenFolder,
  onEditWithNotepad,
  onStartRename,
  onPinResult,
  onRunAsAdmin,
  onCopyPath,
  onCopyFile,
  onQuickLook,
  onExcludeFolder,
  onCopyText,
  onShowProperties,
}) => {
  const { t } = useTranslation();
  const menuRef = React.useRef<HTMLDivElement>(null);
  const [adjustedPos, setAdjustedPos] = React.useState({ x: contextMenu.x, y: contextMenu.y });

  React.useLayoutEffect(() => {
    if (contextMenu.visible && menuRef.current) {
      const rect = menuRef.current.getBoundingClientRect();
      const padding = 10;
      let nextX = contextMenu.x;
      let nextY = contextMenu.y;
      if (nextX + rect.width > window.innerWidth - padding) {
        nextX = Math.max(padding, window.innerWidth - rect.width - padding);
      }
      if (nextY + rect.height > window.innerHeight - padding) {
        nextY = Math.max(padding, window.innerHeight - rect.height - padding);
      }
      setAdjustedPos({ x: nextX, y: nextY });
    } else {
      setAdjustedPos({ x: contextMenu.x, y: contextMenu.y });
    }
  }, [contextMenu.visible, contextMenu.x, contextMenu.y]);

  React.useEffect(() => {
    if (!contextMenu.visible || !menuRef.current) return;
    const items = Array.from(
      menuRef.current.querySelectorAll<HTMLButtonElement>('.search-context-menu-item:not(:disabled)')
    );
    items[0]?.focus({ preventScroll: true });
  }, [contextMenu.visible, contextMenu.result]);

  const handleMenuKeyDown = (e: React.KeyboardEvent<HTMLDivElement>) => {
    if (!menuRef.current) return;
    const items = Array.from(
      menuRef.current.querySelectorAll<HTMLButtonElement>('.search-context-menu-item:not(:disabled)')
    );
    if (items.length === 0) return;
    const activeIdx = items.indexOf(document.activeElement as HTMLButtonElement);
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      e.stopPropagation();
      const next = activeIdx < 0 ? 0 : (activeIdx + 1) % items.length;
      items[next]?.focus();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      e.stopPropagation();
      const next = activeIdx <= 0 ? items.length - 1 : activeIdx - 1;
      items[next]?.focus();
    } else if (e.key === 'Home') {
      e.preventDefault();
      e.stopPropagation();
      items[0]?.focus();
    } else if (e.key === 'End') {
      e.preventDefault();
      e.stopPropagation();
      items[items.length - 1]?.focus();
    }
  };

  if (!contextMenu.visible || !contextMenu.result) {
    return null;
  }

  const res = contextMenu.result;
  const resultType = res.resultType ?? 'file';
  const isFileItem = resultType === 'file';
  const isCommandItem = resultType === 'command';
  const isBookmarkItem = resultType === 'bookmark';
  const isAppItem = resultType === 'app';
  const isDiskBackedItem = isFileItem || isAppItem;
  const displayName = isCommandItem && res.titleKey
    ? (t(res.titleKey as never, res.name as never) as unknown as string)
    : res.name;
  const canRunAsAdmin = isCommandItem
    || isAppItem
    || (!res.isDirectory && /\.(exe|bat|cmd|ps1|msi|vbs|lnk)$/i.test(res.name));

  return (
    <div
      ref={menuRef}
      role="menu"
      className="search-context-menu"
      style={{ left: adjustedPos.x, top: adjustedPos.y }}
      onKeyDown={handleMenuKeyDown}
      onClick={(e) => e.stopPropagation()}
      onContextMenu={(e) => e.preventDefault()}
    >
      <div className="search-context-menu-header">
        <span className="search-context-menu-filename" title={res.commandHint || res.path}>
          {displayName}
        </span>
      </div>

      <div className="search-context-menu-group">
        <button
          type="button"
          className="search-context-menu-item"
          onClick={() => {
            onClose();
            onOpen(res);
          }}
        >
          <FolderOpen size={14} className="menu-icon" />
          <span className="menu-label">
            {isCommandItem
              ? t('search.menuRunCommand', 'Run Command')
              : isAppItem
                ? t('search.menuLaunchApp', 'Launch App')
                : isBookmarkItem
                  ? t('search.menuOpenBookmark', 'Open in Browser')
                  : `${t('search.menuOpen', 'Open')} ${res.isDirectory ? t('search.folderLabel', 'Folder') : t('search.fileLabel', 'File')}`}
          </span>
          <kbd className="menu-shortcut">Enter</kbd>
        </button>

        {isDiskBackedItem && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onOpenFolder(res);
            }}
          >
            <HardDrive size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuOpenFolder', 'Open Containing Folder')}</span>
            <kbd className="menu-shortcut">Ctrl+Enter</kbd>
          </button>
        )}

        {/* 快速预览 */}
        {isFileItem && !res.isDirectory && onQuickLook && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onQuickLook(res);
            }}
          >
            <Eye size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuQuickLook', 'QuickLook Preview')}</span>
            <kbd className="menu-shortcut">F3</kbd>
          </button>
        )}

        {/* 在记事本中编辑 */}
        {isFileItem && !res.isDirectory && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onEditWithNotepad(res);
            }}
            title={t('search.notepadEditTooltip', 'View or edit file with Windows Notepad')}
          >
            <FileCode size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuEditNotepad', 'Edit in Notepad')}</span>
          </button>
        )}

        {/* 重命名 */}
        {isFileItem && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onStartRename(res);
            }}
            title={t('search.renameTooltip', 'Rename file or folder (F2)')}
          >
            <Pencil size={14} className="menu-icon" />
            <span className="menu-label">{t('search.rename', 'Rename')}</span>
            <kbd className="menu-shortcut">F2</kbd>
          </button>
        )}

        {/* 查看文件占用 / 解锁（仅对普通文件/文件夹显示，由主进程 system.openFileLocksmith 托管） */}
        {isFileItem && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              void bridgeRequest('system.openFileLocksmith', { path: res.path }).catch(() => undefined);
            }}
            title={t('search.menuFileLocksmithTip', 'Inspect processes locking this file and unlock it safely')}
          >
            <Unlock size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuFileLocksmith', 'Check File Locks / Unlock')}</span>
          </button>
        )}

        {/* 一键从搜索中排除该文件夹/所在目录 */}
        {isFileItem && onExcludeFolder && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onExcludeFolder(res);
            }}
            title={
              res.isDirectory
                ? t('search.menuExcludeThisFolderTip', 'Hide this folder and all nested files from future search results')
                : t('search.menuExcludeParentFolderTip', 'Hide the containing folder and all nested files from future search results')
            }
          >
            <FolderX size={14} className="menu-icon" />
            <span className="menu-label">
              {res.isDirectory
                ? t('search.menuExcludeThisFolder', 'Exclude This Folder from Search')
                : t('search.menuExcludeParentFolder', 'Exclude Containing Folder from Search')}
            </span>
          </button>
        )}

        {/* 图片独立贴图 */}
        {isFileItem && !res.isDirectory && IMAGE_EXTENSIONS.test(res.name) && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onPinResult(res);
            }}
          >
            <Sparkles size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuPin', 'Pin to Desktop')}</span>
            <kbd className="menu-shortcut">Ctrl+T</kbd>
          </button>
        )}

        {/* 以管理员身份运行 */}
        {canRunAsAdmin && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onRunAsAdmin(res);
            }}
          >
            <ShieldAlert size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuRunAdmin', 'Run as Administrator')}</span>
            <kbd className="menu-shortcut">Ctrl+Shift+Enter</kbd>
          </button>
        )}
      </div>

      <div className="search-context-menu-divider" />

      <div className="search-context-menu-group">
        {isDiskBackedItem && onCopyFile && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onCopyFile(res);
            }}
          >
            <ClipboardCopy size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuCopyFile', 'Copy File')}</span>
            <kbd className="menu-shortcut">Ctrl+C</kbd>
          </button>
        )}

        <button
          type="button"
          className="search-context-menu-item"
          onClick={() => {
            onClose();
            if (isCommandItem && res.commandHint) {
              onCopyText(res.commandHint);
            } else {
              onCopyPath(res);
            }
          }}
        >
          <Copy size={14} className="menu-icon" />
          <span className="menu-label">
            {isCommandItem
              ? t('search.menuCopyCommand', 'Copy Command')
              : isBookmarkItem
                ? t('search.menuCopyUrl', 'Copy URL')
                : t('search.menuCopyPath', 'Copy Full Path')}
          </span>
          <kbd className="menu-shortcut">{isDiskBackedItem ? 'Ctrl+Shift+C' : 'Ctrl+C'}</kbd>
        </button>

        <button
          type="button"
          className="search-context-menu-item"
          onClick={() => {
            onClose();
            onCopyText(displayName);
          }}
        >
          <FileText size={14} className="menu-icon" />
          <span className="menu-label">{t('search.menuCopyName', 'Copy Filename')}</span>
        </button>

        {isDiskBackedItem && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onCopyText(extractParentDirectoryPath(res.path));
            }}
          >
            <Folder size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuCopyParent', 'Copy Parent Directory')}</span>
          </button>
        )}
      </div>

      {isDiskBackedItem && (
        <>
          <div className="search-context-menu-divider" />

          <div className="search-context-menu-group">
            <button
              type="button"
              className="search-context-menu-item"
              onClick={() => {
                onClose();
                void bridgeRequest('search.showShellContextMenu', {
                  filepath: res.path,
                  path: res.path,
                  extended: false,
                });
              }}
              title={t('search.nativeExplorerTooltip', 'Open native Windows Explorer context menu (Shift+Right Click)')}
            >
              <AppWindow size={14} className="menu-icon" />
              <span className="menu-label">{t('search.menuNativeExplorer', 'More Windows Explorer Menu...')}</span>
              <kbd className="menu-shortcut">{t('search.menuShortcutShiftRightClick', 'Shift+Right Click')}</kbd>
            </button>

            <button
              type="button"
              className="search-context-menu-item"
              onClick={() => {
                onClose();
                onShowProperties(res);
              }}
            >
              <Info size={14} className="menu-icon" />
              <span className="menu-label">{t('search.menuProperties', 'Properties')}</span>
              <kbd className="menu-shortcut">Alt+Enter</kbd>
            </button>
          </div>
        </>
      )}
    </div>
  );
};

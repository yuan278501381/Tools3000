import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  Folder,
  FolderOpen,
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

  if (!contextMenu.visible || !contextMenu.result) {
    return null;
  }

  const res = contextMenu.result;

  return (
    <div
      ref={menuRef}
      className="search-context-menu"
      style={{ left: adjustedPos.x, top: adjustedPos.y }}
      onClick={(e) => e.stopPropagation()}
      onContextMenu={(e) => e.preventDefault()}
    >
      <div className="search-context-menu-header">
        <span className="search-context-menu-filename" title={res.path}>
          {res.name}
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
          <span className="menu-label">{t('search.menuOpen', 'Open')} {res.isDirectory ? t('search.folderLabel', 'Folder') : t('search.fileLabel', 'File')}</span>
          <kbd className="menu-shortcut">Enter</kbd>
        </button>

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

        {/* 快速预览 */}
        {!res.isDirectory && onQuickLook && (
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
        {!res.isDirectory && (
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

        {/* 图片独立贴图 */}
        {!res.isDirectory && IMAGE_EXTENSIONS.test(res.name) && (
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
        {!res.isDirectory && /\.(exe|bat|cmd|ps1|msi|vbs)$/i.test(res.name) && (
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
        <button
          type="button"
          className="search-context-menu-item"
          onClick={() => {
            onClose();
            onCopyPath(res);
          }}
        >
          <Copy size={14} className="menu-icon" />
          <span className="menu-label">{t('search.menuCopyPath', 'Copy Full Path')}</span>
          <kbd className="menu-shortcut">Ctrl+C</kbd>
        </button>

        {onCopyFile && (
          <button
            type="button"
            className="search-context-menu-item"
            onClick={() => {
              onClose();
              onCopyFile(res);
            }}
          >
            <ClipboardCopy size={14} className="menu-icon" />
            <span className="menu-label">{t('search.menuCopyFile', 'Copy File Object (Paste to Chat/Folder)')}</span>
            <kbd className="menu-shortcut">Ctrl+Shift+C</kbd>
          </button>
        )}

        <button
          type="button"
          className="search-context-menu-item"
          onClick={() => {
            onClose();
            onCopyText(res.name);
          }}
        >
          <FileText size={14} className="menu-icon" />
          <span className="menu-label">{t('search.menuCopyName', 'Copy Filename')}</span>
        </button>

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
      </div>

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
    </div>
  );
};

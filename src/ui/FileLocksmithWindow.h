#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FileLocksmithWindow.h — 纯原生 Direct2D 零常驻文件占用解锁弹窗
//
// 特性:
//   - 由 Tools3000.exe --unlock "<path>" 独立拉起，冷启动 <25ms、峰值 ~8MB、关闭 0 驻留；
//   - 全支持深浅主题自适应、高分屏 DPI 缩放、Win10/11 通用圆角、8 语种 UiI18n 与零 Emoji。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_FILELOCKSMITHWINDOW_H
#define TOOLS3000_UI_FILELOCKSMITHWINDOW_H

#include <windows.h>
#include <string>

namespace tools3000::ui {

/// 阻塞运行独立 File Locksmith 原生窗口消息循环直至用户关闭，返回退出码 0
int runFileLocksmithWindow(HINSTANCE hInstance, const std::wstring& targetPath);

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_FILELOCKSMITHWINDOW_H

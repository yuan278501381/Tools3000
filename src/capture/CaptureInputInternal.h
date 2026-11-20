#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CaptureInputInternal.h — 截图输入处理器私有跨编译单元协作声明
//
// 职责:
//   仅供 CaptureInput*.cpp 内部拆分编译单元共享：
//   - 渲染定时器常量与比例/取色/光标/保存格式/合并撤销辅助函数声明（唯一定义位于 CaptureInput.cpp）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_CAPTUREINPUTINTERNAL_H
#define TOOLS3000_CAPTURE_CAPTUREINPUTINTERNAL_H

#include "capture/CaptureInput.h"
#include <filesystem>
#include <string>

#ifndef RENDER_TIMER_ID
#define RENDER_TIMER_ID 1
#endif

namespace tools3000::capture::input_detail {

float getTargetAspectRatio(AspectRatioPreset preset);
std::string formatColorText(ColorFormatType type, int r, int g, int b);
HCURSOR cursorForArea(HitArea area, int cornerIndex = 0, float dpiScale = 1.0f, bool isDragging = false);
DWORD filterIndexForFormat(ImageFormat format);
const wchar_t* extensionForFormat(ImageFormat format);
ImageFormat formatForSavePath(const std::filesystem::path& path, DWORD filterIndex);
void pushCoalescedElementUndo(CaptureState& state, DWORD coalesceWindowMs = 450);
void resetCoalescedUndo(CaptureState& state) noexcept;

} // namespace tools3000::capture::input_detail

#endif // TOOLS3000_CAPTURE_CAPTUREINPUTINTERNAL_H

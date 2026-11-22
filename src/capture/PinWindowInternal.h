#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinWindowInternal.h — PinWindow 私有跨编译单元内部协作声明
//
// 职责:
//   仅供 PinWindow*.cpp 内部拆分编译单元共享：
//   - 窗口类名常量与交互按钮枚举
//   - 纯无状态几何/环境内联小辅助函数
//   - 跨编译单元共享原子量 extern 声明（唯一定义位于 PinWindow.cpp，严守 ODR）
//   - 跨子域内部辅助函数声明（各自在所属子域 .cpp 唯一定义）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINWINDOWINTERNAL_H
#define TOOLS3000_CAPTURE_PINWINDOWINTERNAL_H

#include "capture/PinWindow.h"
#include "capture/PinContextMenuModel.h"
#include "capture/TextReplaceEngine.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::capture::pin_detail {

inline constexpr const wchar_t* kPinWindowClass = L"Tools3000_PinWindow";

enum PinInteractiveButton : int {
    kPinBtnNone = -1,
    kPinBtnFold = 0,
    kPinBtnRotate = 1,
    kPinBtnInvert = 2,
    kPinBtnEdit = 3,
    kPinBtnText = 4,
    kPinBtnTranslate = 5,
    kPinBtnCopy = 6,
    kPinBtnSave = 7,
    kPinBtnClose = 8,
    kPinBtnGroupBadge = 9,
    kPinBtnCopyFloating = 10,
    kPinBtnCopyTableFloating = 11,
    kPinBtnOpenPanelFloating = 12,
    kPinBtnHighlightFloating = 13,
    kPinBtnWavyFloating = 14,
    kPinBtnUnderlineFloating = 15,
    kPinBtnUndoFloating = 16,
    kPinBtnTextReplaceFloating = 17,
    kPinBtnTranslateFloating = 18,
    kPinBtnTranslateStatus = 19,
    kPinBtnColorDotBase = 20, // 20..24
    kPinBtnTrFontDec = 30,
    kPinBtnTrFontInc = 31,
    kPinBtnTrWeight = 32,
    kPinBtnTrFamily = 33,
    kPinBtnTrBgMode = 34,
    kPinBtnTrDone = 35,
    kPinBtnTrReflow = 36,
    kPinBtnQrBadgeBase = 100,
};

inline bool isPointInRectF(const D2D1_RECT_F& rc, int x, int y) noexcept {
    return (rc.right > rc.left && rc.bottom > rc.top &&
            static_cast<float>(x) >= rc.left && static_cast<float>(x) <= rc.right &&
            static_cast<float>(y) >= rc.top && static_cast<float>(y) <= rc.bottom);
}

inline bool isAutomatedTestProcessMode() noexcept {
    wchar_t buf[8]{};
    const DWORD len = GetEnvironmentVariableW(
        L"TOOLS3000_TEST_MODE", buf, static_cast<DWORD>(std::size(buf)));
    return len > 0 && buf[0] == L'1';
}

// 唯一定义位于 PinWindow.cpp（严格 ODR，杜绝多编译单元静态状态分裂）
extern std::atomic<bool> s_ocrShuttingDown;
extern std::atomic<uint32_t> s_globalActiveOcrWorkers;

// 剪贴板与保存辅助（唯一定义位于 PinWindow.cpp）
bool copyImageToClipboard(const cv::Mat& image, HWND ownerHwnd = nullptr);
bool savePinnedImage(HWND owner, const cv::Mat& image);

// OCR 块规整、字符定位与选区拼接辅助（唯一定义位于 PinWindowOcr.cpp）
void cleanupOcrWorkingSet();
void normalizePinOcrBlocksAndTableState(
    std::vector<PinOcrBlock>& blocks,
    const cv::Mat& sourceImage,
    bool& outHasTable);
size_t locateNearestCharInBlock(const PinOcrBlock& blk, float mx) noexcept;
std::string joinAlignedSelectedPieces(
    std::vector<tools3000::ocr::OcrResult>& pieces,
    const char* lineEnding);
std::string assembleSelectedSpansText(
    const std::vector<PinOcrBlock>& blocks,
    const std::vector<size_t>& selectedBlockIndices,
    const std::vector<PinBlockCharSpan>& selectedCharSpans);

// 原位改字编辑叠加参数构造（唯一定义位于 PinWindowTextReplace.cpp）
TextReplaceRenderSpec buildTextReplaceSpecForOverlay(const MarkupElement& el);

// 右键菜单快照与命令执行（唯一定义位于 PinWindowInput.cpp）
PinContextMenuSnapshot buildPinContextMenuSnapshot(const PinWindow* self);
void executePinContextMenuCommand(PinWindow* self, HWND hwnd, const PinMenuCommand& cmd);

} // namespace tools3000::capture::pin_detail

#endif // TOOLS3000_CAPTURE_PINWINDOWINTERNAL_H

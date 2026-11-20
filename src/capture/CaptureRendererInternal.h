#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CaptureRendererInternal.h — 截图渲染器私有跨编译单元协作声明
//
// 职责:
//   仅供 CaptureRenderer*.cpp 内部拆分编译单元共享：
//   - 色彩格式换算条目、取色格式化辅助与 DirectWrite TextFormatScope RAII 守卫
//   - 美化套壳渐变配色辅助声明（唯一定义位于 CaptureRendererToolbar.cpp）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_CAPTURERENDERERINTERNAL_H
#define TOOLS3000_CAPTURE_CAPTURERENDERERINTERNAL_H

#include "capture/CaptureRenderer.h"
#include <dwrite.h>
#include <string>
#include <vector>

namespace tools3000::capture::renderer_detail {

bool isCaptureUiChinese();
void rgbToHsl(int r, int g, int b, int& h, int& s, int& l);
void rgbToHsv(int r, int g, int b, int& h, int& s, int& v);
void rgbToCmyk(int r, int g, int b, int& c, int& m, int& y, int& k);

struct ColorFormatEntry {
    ColorFormatType type;
    const wchar_t* tag;
    std::wstring value;
    std::string clipText;
};

std::vector<ColorFormatEntry> getAllColorFormats(int r, int g, int b);

/// DirectWrite 文本格式作用域保护 RAII，确保文本对齐、段落对齐与换行策略不跨模块污染
struct TextFormatScope {
    IDWriteTextFormat* format = nullptr;
    DWRITE_TEXT_ALIGNMENT origText = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT origPara = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    DWRITE_WORD_WRAPPING origWrap = DWRITE_WORD_WRAPPING_NO_WRAP;

    TextFormatScope(IDWriteTextFormat* fmt,
                    DWRITE_TEXT_ALIGNMENT text = DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                    DWRITE_WORD_WRAPPING wrap = DWRITE_WORD_WRAPPING_NO_WRAP)
        : format(fmt) {
        if (format) {
            origText = format->GetTextAlignment();
            origPara = format->GetParagraphAlignment();
            origWrap = format->GetWordWrapping();
            format->SetTextAlignment(text);
            format->SetParagraphAlignment(para);
            format->SetWordWrapping(wrap);
        }
    }

    ~TextFormatScope() {
        if (format) {
            format->SetTextAlignment(origText);
            format->SetParagraphAlignment(origPara);
            format->SetWordWrapping(origWrap);
        }
    }
};

void getBeautyColors(BeautyBackgroundType type, D2D1_COLOR_F& c1, D2D1_COLOR_F& c2, D2D1_COLOR_F& c3);

} // namespace tools3000::capture::renderer_detail

#endif // TOOLS3000_CAPTURE_CAPTURERENDERERINTERNAL_H

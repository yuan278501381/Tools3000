#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// DWriteFontUtils.h — DirectWrite 真实已安装字体探测与原生浮层语言判定单一事实源
//
// 工程背景:
//   IDWriteFactory::CreateTextFormat 即便传入系统不存在的字体名也会恒定返回 S_OK，
//   在光栅化阶段回退至粗糙的底层字符映射表。本工具通过 GetSystemFontCollection
//   与 FindFamilyName 验证真实安装状态，确保中英混排基线与字重浑然一体。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_DWRITEFONTUTILS_H
#define TOOLS3000_CORE_UTILS_DWRITEFONTUTILS_H

#include "core/config/ConfigManager.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"

#include <dwrite.h>
#include <wrl/client.h>
#include <string>

namespace tools3000::core {

/// 探测系统真实已安装的无衬线 UI 字体家族（优先思源黑体，平滑降级微软雅黑 UI / Segoe UI）
inline const wchar_t* resolveInstalledUiFontFamily(IDWriteFactory* dwriteFactory) noexcept {
    if (dwriteFactory) {
        Microsoft::WRL::ComPtr<IDWriteFontCollection> fontCollection;
        if (SUCCEEDED(dwriteFactory->GetSystemFontCollection(fontCollection.GetAddressOf(), FALSE)) && fontCollection) {
            static constexpr const wchar_t* kUiCandidates[] = {
                L"Noto Sans SC",
                L"Source Han Sans SC",
                L"Microsoft YaHei UI",
                L"Segoe UI Variable Text",
                L"Segoe UI"
            };
            for (const wchar_t* candidate : kUiCandidates) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fontCollection->FindFamilyName(candidate, &index, &exists)) && exists) {
                    return candidate;
                }
            }
        }
    }
    return L"Microsoft YaHei UI";
}

/// 探测系统真实已安装的现代等宽字体家族（仅用于纯英文/数字码值展示；含中文文案请使用 resolveInstalledUiFontFamily）
inline const wchar_t* resolveInstalledMonoFontFamily(IDWriteFactory* dwriteFactory) noexcept {
    if (dwriteFactory) {
        Microsoft::WRL::ComPtr<IDWriteFontCollection> fontCollection;
        if (SUCCEEDED(dwriteFactory->GetSystemFontCollection(fontCollection.GetAddressOf(), FALSE)) && fontCollection) {
            static constexpr const wchar_t* kMonoCandidates[] = {
                L"Cascadia Code",
                L"Cascadia Mono",
                L"Consolas",
                L"Microsoft YaHei UI"
            };
            for (const wchar_t* candidate : kMonoCandidates) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fontCollection->FindFamilyName(candidate, &index, &exists)) && exists) {
                    return candidate;
                }
            }
        }
    }
    return L"Consolas";
}

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_UTILS_DWRITEFONTUTILS_H

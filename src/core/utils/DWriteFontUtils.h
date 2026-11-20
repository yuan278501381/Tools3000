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

/// 探测系统真实已安装的无衬线 UI 字体家族（按当前 8 语种智能优选原生字体，0 安装包膨胀）
inline const wchar_t* resolveInstalledUiFontFamily(IDWriteFactory* dwriteFactory) noexcept {
    const UiLanguage lang = UiI18n::getEffectiveLanguage();
    auto findFirstInstalled = [&](const wchar_t* const* candidates, std::size_t count, const wchar_t* fallback) noexcept {
        if (dwriteFactory) {
            Microsoft::WRL::ComPtr<IDWriteFontCollection> fontCollection;
            if (SUCCEEDED(dwriteFactory->GetSystemFontCollection(fontCollection.GetAddressOf(), FALSE)) && fontCollection) {
                for (std::size_t i = 0; i < count; ++i) {
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    if (SUCCEEDED(fontCollection->FindFamilyName(candidates[i], &index, &exists)) && exists) {
                        return candidates[i];
                    }
                }
            }
        }
        return fallback;
    };

    switch (lang) {
        case UiLanguage::ZhTW: {
            static constexpr const wchar_t* kTw[] = {
                L"Microsoft JhengHei UI", L"Noto Sans TC", L"Segoe UI Variable Text", L"Segoe UI", L"Microsoft YaHei UI"
            };
            return findFirstInstalled(kTw, std::size(kTw), L"Microsoft JhengHei UI");
        }
        case UiLanguage::JaJP: {
            static constexpr const wchar_t* kJa[] = {
                L"Yu Gothic UI", L"Meiryo UI", L"Noto Sans JP", L"Segoe UI Variable Text", L"Segoe UI"
            };
            return findFirstInstalled(kJa, std::size(kJa), L"Yu Gothic UI");
        }
        case UiLanguage::KoKR: {
            static constexpr const wchar_t* kKo[] = {
                L"Malgun Gothic", L"Noto Sans KR", L"Segoe UI Variable Text", L"Segoe UI"
            };
            return findFirstInstalled(kKo, std::size(kKo), L"Malgun Gothic");
        }
        case UiLanguage::EnUS:
        case UiLanguage::RuRU:
        case UiLanguage::PtBR:
        case UiLanguage::EsES: {
            static constexpr const wchar_t* kLatinCyr[] = {
                L"Segoe UI Variable Text", L"Segoe UI", L"Noto Sans SC", L"Microsoft YaHei UI"
            };
            return findFirstInstalled(kLatinCyr, std::size(kLatinCyr), L"Segoe UI");
        }
        case UiLanguage::ZhCN:
        default: {
            static constexpr const wchar_t* kZh[] = {
                L"Noto Sans SC", L"Source Han Sans SC", L"Microsoft YaHei UI", L"Segoe UI Variable Text", L"Segoe UI"
            };
            return findFirstInstalled(kZh, std::size(kZh), L"Microsoft YaHei UI");
        }
    }
}

/// 返回供 DirectWrite CreateTextFormat 使用的当前有效 BCP-47 区域名（保障简繁/日韩本地字形整形）
inline const wchar_t* resolveInstalledUiLocaleNameW() noexcept {
    return UiI18n::toLocaleNameW(UiI18n::getEffectiveLanguage());
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

#include "capture/CaptureDropdownMenu.h"

#include "core/config/ConfigManager.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace tools3000::capture {

namespace {

const char* recordFormatConfigId(RecordFormat fmt) noexcept {
    switch (fmt) {
        case RecordFormat::MP4_H265: return "mp4_h265";
        case RecordFormat::WebM_VP9: return "webm_vp9";
        case RecordFormat::GIF:      return "gif";
        default:                     return "mp4_h264";
    }
}

RecordFormat resolveRememberedVideoFormat(const CaptureState& state) {
    if (state.recordSetup.format != RecordFormat::GIF) {
        return state.recordSetup.format;
    }
    const std::string saved = tools3000::core::ConfigManager::instance().get<std::string>(
        "/recording/videoFormat", "");
    if (saved == "mp4_h265") return RecordFormat::MP4_H265;
    if (saved == "webm_vp9") return RecordFormat::WebM_VP9;
    if (saved == "mp4_h264") return RecordFormat::MP4_H264;
    if (state.recordSetup.preferredVideoFormat != RecordFormat::GIF) {
        return state.recordSetup.preferredVideoFormat;
    }
    return RecordFormat::MP4_H264;
}

} // namespace

void openDropdownMenu(CaptureState& state, DropdownType type, const D2D1_RECT_F& anchorRect) {
    if (anchorRect.right <= anchorRect.left) {
        // 纯快捷键触发，无按钮物理矩形：直接轮转下一项
        if (type == DropdownType::BeautyShellBg) {
            int cur = static_cast<int>(state.beautyShell.bgType);
            int next = (cur + 1) % static_cast<int>(BeautyBackgroundType::COUNT);
            commitDropdownSelection(state, DropdownType::BeautyShellBg, next);
            return;
        } else if (type == DropdownType::BeautyShellPadding) {
            static constexpr std::array<int, 6> kPaddings = { 16, 24, 32, 48, 64, 72 };
            int curPad = state.beautyShell.padding;
            int nextPad = kPaddings[0];
            for (size_t i = 0; i < kPaddings.size(); ++i) {
                if (kPaddings[i] > curPad) {
                    nextPad = kPaddings[i];
                    break;
                }
            }
            commitDropdownSelection(state, DropdownType::BeautyShellPadding, nextPad);
            return;
        } else if (type == DropdownType::BeautyShellRadius) {
            static const std::array<float, 7> radiuses = { 0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f };
            float cur = state.beautyShell.cornerRadius;
            int nextIdx = 0;
            for (size_t i = 0; i < radiuses.size(); ++i) {
                if (radiuses[i] > cur + 0.5f) { nextIdx = static_cast<int>(i); break; }
            }
            commitDropdownSelection(state, DropdownType::BeautyShellRadius, static_cast<int>(radiuses[nextIdx]));
            return;
        } else if (type == DropdownType::NumberFormat) {
            int cur = static_cast<int>(state.currentNumberFormat);
            int next = (cur + 1) % static_cast<int>(NumberFormat::COUNT);
            commitDropdownSelection(state, DropdownType::NumberFormat, next);
            return;
        } else if (type == DropdownType::ShapeMode) {
            int next = (state.currentTool == MarkupTool::Spotlight) ? 0 : 1;
            commitDropdownSelection(state, DropdownType::ShapeMode, next);
            return;
        }
    }

    if (state.dropdownMenu.type == type) {
        closeDropdownMenu(state, true);
        return;
    }

    // 互斥收起其他已展开的滑块、子菜单或尺寸菜单，杜绝多弹出面板叠压
    state.sliderPopup.type = SliderPopupType::None;
    state.sliderPopup.isDragging = false;
    state.openSubmenu = SubmenuType::None;
    state.isSizeMenuOpen = false;

    state.dropdownMenu.type = type;
    state.dropdownMenu.anchorButtonRect = anchorRect;
    state.dropdownMenu.hoveredIndex = -1;
    state.dropdownMenu.items.clear();
    state.dropdownMenu.originalBgType = state.beautyShell.bgType;
    const bool isZh = tools3000::core::isUiLanguageChinese();

    switch (type) {
        case DropdownType::BeautyShellBg: {
            state.dropdownMenu.items = {
                { 0, isZh ? L"极光紫" : L"Aurora Purple", isZh ? L"流光紫粉" : L"Indigo Rose",
                  state.beautyShell.bgType == BeautyBackgroundType::StudioSlate, false, {},
                  true, D2D1::ColorF(99.0f/255.0f, 102.0f/255.0f, 241.0f/255.0f), D2D1::ColorF(236.0f/255.0f, 72.0f/255.0f, 153.0f/255.0f) },
                { 1, isZh ? L"日落珊瑚" : L"Sunrise Coral", isZh ? L"珊瑚暖金" : L"Coral Gold",
                  state.beautyShell.bgType == BeautyBackgroundType::PaperChalk, false, {},
                  true, D2D1::ColorF(251.0f/255.0f, 113.0f/255.0f, 133.0f/255.0f), D2D1::ColorF(250.0f/255.0f, 204.0f/255.0f, 21.0f/255.0f) },
                { 2, isZh ? L"翡翠海" : L"Emerald Sea", isZh ? L"深海翡翠" : L"Cyan Emerald",
                  state.beautyShell.bgType == BeautyBackgroundType::SilkMist, false, {},
                  true, D2D1::ColorF(14.0f/255.0f, 165.0f/255.0f, 233.0f/255.0f), D2D1::ColorF(16.0f/255.0f, 185.0f/255.0f, 129.0f/255.0f) },
                { 3, isZh ? L"曜石黑" : L"Obsidian Dark", isZh ? L"极客深邃" : L"Dark Carbon",
                  state.beautyShell.bgType == BeautyBackgroundType::MidnightGraphite, false, {},
                  true, D2D1::ColorF(51.0f/255.0f, 65.0f/255.0f, 85.0f/255.0f), D2D1::ColorF(9.0f/255.0f, 13.0f/255.0f, 22.0f/255.0f) },
                { 4, isZh ? L"极简浅灰" : L"Minimal Light", isZh ? L"质感银灰" : L"Silver Matte",
                  state.beautyShell.bgType == BeautyBackgroundType::PureMinimal, false, {},
                  true, D2D1::ColorF(248.0f/255.0f, 250.0f/255.0f, 252.0f/255.0f), D2D1::ColorF(203.0f/255.0f, 213.0f/255.0f, 225.0f/255.0f) }
            };
            break;
        }

        case DropdownType::BeautyShellPadding: {
            state.dropdownMenu.items = {
                { 16, isZh ? L"极简留白" : L"Minimal",  L"16 px", state.beautyShell.padding == 16 },
                { 24, isZh ? L"紧凑留白" : L"Compact",  L"24 px", state.beautyShell.padding == 24 },
                { 32, isZh ? L"标准均衡" : L"Balanced", L"32 px", state.beautyShell.padding == 32 },
                { 48, isZh ? L"舒适自然" : L"Standard", L"48 px", state.beautyShell.padding == 48 },
                { 64, isZh ? L"宽阔景深" : L"Wide",     L"64 px", state.beautyShell.padding == 64 },
                { 72, isZh ? L"大气呼吸" : L"Spacious", L"72 px", state.beautyShell.padding == 72 }
            };
            break;
        }

        case DropdownType::BeautyShellRadius: {
            int curR = static_cast<int>(std::round(state.beautyShell.cornerRadius));
            state.dropdownMenu.items = {
                { 0,  isZh ? L"硬朗直角" : L"Sharp",    L"0 px",  curR == 0 },
                { 8,  isZh ? L"利落微角" : L"Crisp",    L"8 px",  curR == 8 },
                { 10, isZh ? L"轻微圆润" : L"Subtle",   L"10 px", curR == 10 },
                { 12, isZh ? L"现代柔和" : L"Soft",     L"12 px", curR == 12 },
                { 16, isZh ? L"标准优雅" : L"Standard", L"16 px", curR == 16 },
                { 24, isZh ? L"大弧流线" : L"Rounded",  L"24 px", curR == 24 },
                { 32, isZh ? L"胶囊视界" : L"Curved",   L"32 px", curR == 32 }
            };
            break;
        }

        case DropdownType::RecordFormat: {
            const RecordFormat prefVideo = resolveRememberedVideoFormat(state);
            std::wstring videoLabel = isZh ? L"MP4 视频" : L"MP4 Video";
            std::wstring videoHint = L"H.264 / AAC";
            if (prefVideo == RecordFormat::MP4_H265) {
                videoLabel = isZh ? L"MP4 (HEVC)" : L"MP4 (HEVC)";
                videoHint = L"H.265 / AAC";
            } else if (prefVideo == RecordFormat::WebM_VP9) {
                videoLabel = isZh ? L"WebM 视频" : L"WebM Video";
                videoHint = L"VP9 / Opus";
            }
            state.dropdownMenu.items = {
                { 0, videoLabel, videoHint, state.recordSetup.format != RecordFormat::GIF },
                { 1, isZh ? L"GIF 动图" : L"GIF Animation", isZh ? L"微信 20MB 保护" : L"WeChat 20MB Safe", state.recordSetup.format == RecordFormat::GIF }
            };
            break;
        }

        case DropdownType::RecordFps: {
            state.dropdownMenu.items = {
                { 15, L"15 fps", isZh ? L"省流极简" : L"Low Size", state.recordSetup.fps == 15 },
                { 30, L"30 fps", isZh ? L"推荐流畅" : L"Balanced", state.recordSetup.fps == 30 },
                { 60, L"60 fps", isZh ? L"极客高刷" : L"Ultra Smooth", state.recordSetup.fps == 60 }
            };
            break;
        }

        case DropdownType::RecordQuality: {
            const int curMbps = state.recordSetup.bitrateMbps;
            state.dropdownMenu.items = {
                { 0, isZh ? L"流畅画质" : L"Smooth",          L"4 Mbps",  curMbps == 4 || (curMbps <= 0 && state.recordSetup.qualityLevel == 0) },
                { 1, isZh ? L"高清标准" : L"HD Standard",     L"8 Mbps",  curMbps == 8 || (curMbps <= 0 && state.recordSetup.qualityLevel == 1) },
                { 2, isZh ? L"超清高码" : L"Ultra High",      L"16 Mbps", curMbps == 16 || (curMbps <= 0 && state.recordSetup.qualityLevel == 2) },
                { 3, isZh ? L"无损级原画" : L"Lossless Master", L"32 Mbps", curMbps == 32 || (curMbps <= 0 && state.recordSetup.qualityLevel >= 3) }
            };
            break;
        }

        case DropdownType::RecordClickEffect: {
            bool ripple = state.recordSetup.showClickEffects;
            bool zoom = state.recordSetup.showClickZoom;
            int curMode = (!ripple && !zoom) ? 0 : (ripple && !zoom ? 1 : (!ripple && zoom ? 2 : 3));
            state.dropdownMenu.items = {
                { 0, isZh ? L"无点击反馈" : L"None", isZh ? L"保持纯净" : L"Pure Clean", curMode == 0 },
                { 1, isZh ? L"水波纹动效" : L"Water Ripple", isZh ? L"扩散涟漪" : L"Ripple Wave", curMode == 1 },
                { 2, isZh ? L"Mac 点击缩放" : L"Mac Click Zoom", isZh ? L"柔和聚焦" : L"Smooth Focus", curMode == 2 },
                { 3, isZh ? L"水波纹 + 缩放" : L"Ripple + Zoom", isZh ? L"双重反馈" : L"Dual Feedback", curMode == 3 }
            };
            break;
        }

        case DropdownType::ShapeMode: {
            const bool isSpotlight = (state.isMarkupToolActive && state.currentTool == MarkupTool::Spotlight);
            const bool isGeoShape = (state.isMarkupToolActive &&
                (state.currentTool == MarkupTool::Rectangle ||
                 state.currentTool == MarkupTool::Ellipse ||
                 state.currentTool == MarkupTool::Line));
            state.dropdownMenu.items = {
                { 0, isZh ? L"几何图形" : L"Shapes",    L"Shift+1", isGeoShape },
                { 1, isZh ? L"聚光灯"   : L"Spotlight", L"Shift+0", isSpotlight }
            };
            break;
        }

        case DropdownType::NumberFormat: {
            auto fmtSubLabel = [&](NumberFormat fmt, const wchar_t* sample) -> std::wstring {
                const int nextVal = state.markup.currentNumber(fmt);
                if (nextVal > 1) {
                    return isZh
                        ? std::format(L"下一项: {}", formatNumberBadgeLabel(nextVal, fmt))
                        : std::format(L"Next: {}", formatNumberBadgeLabel(nextVal, fmt));
                }
                return sample;
            };
            state.dropdownMenu.items = {
                { static_cast<int>(NumberFormat::Arabic),     numberFormatMenuLabel(NumberFormat::Arabic, isZh),     fmtSubLabel(NumberFormat::Arabic, L"1, 2, 3"),  state.currentNumberFormat == NumberFormat::Arabic },
                { static_cast<int>(NumberFormat::UpperAlpha), numberFormatMenuLabel(NumberFormat::UpperAlpha, isZh), fmtSubLabel(NumberFormat::UpperAlpha, L"A, B, C"),  state.currentNumberFormat == NumberFormat::UpperAlpha },
                { static_cast<int>(NumberFormat::LowerAlpha), numberFormatMenuLabel(NumberFormat::LowerAlpha, isZh), fmtSubLabel(NumberFormat::LowerAlpha, L"a, b, c"),  state.currentNumberFormat == NumberFormat::LowerAlpha },
                { static_cast<int>(NumberFormat::Roman),      numberFormatMenuLabel(NumberFormat::Roman, isZh),      fmtSubLabel(NumberFormat::Roman, L"I, II, III"), state.currentNumberFormat == NumberFormat::Roman },
                { static_cast<int>(NumberFormat::Chinese),    numberFormatMenuLabel(NumberFormat::Chinese, isZh),    fmtSubLabel(NumberFormat::Chinese, isZh ? L"一, 二, 十, 十一" : L"一, 二, 三"), state.currentNumberFormat == NumberFormat::Chinese }
            };
            break;
        }

        default:
            break;
    }

    state.toolbarLayoutValid = false;
}

void commitDropdownSelection(CaptureState& state, DropdownType type, int id) {
    const bool isZh = tools3000::core::isUiLanguageChinese();
    state.loupeToastUntil = GetTickCount() + 1400;

    switch (type) {
        case DropdownType::BeautyShellBg: {
            if (id >= 0 && id < static_cast<int>(BeautyBackgroundType::COUNT)) {
                state.beautyShell.bgType = static_cast<BeautyBackgroundType>(id);
                state.beautyShell.enabled = true;
                state.dropdownMenu.originalBgType = state.beautyShell.bgType;
                const char* themeName = "aurora_purple";
                switch (state.beautyShell.bgType) {
                    case BeautyBackgroundType::StudioSlate: themeName = "aurora_purple"; break;
                    case BeautyBackgroundType::PaperChalk: themeName = "sunrise_coral"; break;
                    case BeautyBackgroundType::SilkMist: themeName = "emerald_sea"; break;
                    case BeautyBackgroundType::MidnightGraphite: themeName = "obsidian_dark"; break;
                    case BeautyBackgroundType::PureMinimal: themeName = "minimal_light"; break;
                    default: break;
                }
                tools3000::core::ConfigManager::instance().set<std::string>("/capture/beautyShellTheme", themeName);
                tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
                static const wchar_t* bgNames[] = { L"极光紫", L"日落珊瑚", L"翡翠海", L"曜石黑", L"极简浅灰" };
                static const wchar_t* bgNamesEn[] = { L"Aurora Purple", L"Sunrise Coral", L"Emerald Sea", L"Obsidian Dark", L"Minimal Light" };
                state.loupeToastMessage = (isZh ? L"外壳质感: " : L"Shell Theme: ") + std::wstring(isZh ? bgNames[id] : bgNamesEn[id]);
            }
            break;
        }

        case DropdownType::BeautyShellPadding: {
            state.beautyShell.padding = id;
            state.beautyShell.enabled = true;
            tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellPadding", id);
            tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
            state.loupeToastMessage = isZh ? std::format(L"外壳留白: {} px", id) : std::format(L"Shell Padding: {} px", id);
            break;
        }

        case DropdownType::BeautyShellRadius: {
            state.beautyShell.cornerRadius = static_cast<float>(id);
            state.beautyShell.enabled = true;
            tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellRadius", id);
            tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
            state.loupeToastMessage = isZh ? std::format(L"外壳圆角: {} px", id) : std::format(L"Shell Radius: {} px", id);
            break;
        }

        case DropdownType::RecordFormat: {
            auto& cfg = tools3000::core::ConfigManager::instance();
            if (id == 1) {
                if (state.recordSetup.format != RecordFormat::GIF) {
                    state.recordSetup.preferredVideoFormat = state.recordSetup.format;
                    cfg.set<std::string>("/recording/videoFormat", recordFormatConfigId(state.recordSetup.format));
                }
                state.recordSetup.format = RecordFormat::GIF;
                cfg.set<std::string>("/recording/format", "gif");
                state.loupeToastMessage = isZh ? L"录屏格式: GIF 动图" : L"Format: GIF";
            } else {
                const RecordFormat restored = resolveRememberedVideoFormat(state);
                state.recordSetup.preferredVideoFormat = restored;
                state.recordSetup.format = restored;
                cfg.set<std::string>("/recording/format", recordFormatConfigId(restored));
                if (restored == RecordFormat::MP4_H265) {
                    state.loupeToastMessage = isZh ? L"录屏格式: MP4 (HEVC)" : L"Format: MP4 (HEVC)";
                } else if (restored == RecordFormat::WebM_VP9) {
                    state.loupeToastMessage = isZh ? L"录屏格式: WebM (VP9)" : L"Format: WebM (VP9)";
                } else {
                    state.loupeToastMessage = isZh ? L"录屏格式: MP4 视频" : L"Format: MP4";
                }
            }
            break;
        }

        case DropdownType::RecordFps: {
            state.recordSetup.fps = id;
            tools3000::core::ConfigManager::instance().set<int>("/recording/fps", id);
            state.loupeToastMessage = isZh ? std::format(L"录制帧率: {} fps", id) : std::format(L"Frame Rate: {} fps", id);
            break;
        }

        case DropdownType::RecordQuality: {
            const int clampedLevel = std::clamp(id, 0, 3);
            const int bitrate = recordQualityLevelToBitrateMbps(clampedLevel);
            state.recordSetup.qualityLevel = clampedLevel;
            state.recordSetup.bitrateMbps = bitrate;
            tools3000::core::ConfigManager::instance().set<int>("/recording/bitrate", bitrate);
            static const wchar_t* qNames[] = { L"流畅 4Mbps", L"高清 8Mbps", L"超清 16Mbps", L"原画 32Mbps" };
            static const wchar_t* qNamesEn[] = { L"4 Mbps", L"8 Mbps", L"16 Mbps", L"32 Mbps" };
            state.loupeToastMessage = (isZh ? L"录制码率: " : L"Bitrate: ") + std::wstring(isZh ? qNames[clampedLevel] : qNamesEn[clampedLevel]);
            break;
        }

        case DropdownType::RecordClickEffect: {
            if (id == 0) {
                state.recordSetup.showClickEffects = false;
                state.recordSetup.showClickZoom = false;
                state.loupeToastMessage = isZh ? L"点击特效: 已关闭" : L"Click Effects: None";
            } else if (id == 1) {
                state.recordSetup.showClickEffects = true;
                state.recordSetup.showClickZoom = false;
                state.loupeToastMessage = isZh ? L"点击特效: 水波纹" : L"Click Effects: Water Ripple";
            } else if (id == 2) {
                state.recordSetup.showClickEffects = false;
                state.recordSetup.showClickZoom = true;
                state.loupeToastMessage = isZh ? L"点击特效: Mac 点击缩放" : L"Click Effects: Mac Click Zoom";
            } else {
                state.recordSetup.showClickEffects = true;
                state.recordSetup.showClickZoom = true;
                state.loupeToastMessage = isZh ? L"点击特效: 水波纹 + 缩放" : L"Click Effects: Ripple + Zoom";
            }
            tools3000::core::ConfigManager::instance().set<bool>("/recording/showClickEffects", state.recordSetup.showClickEffects);
            tools3000::core::ConfigManager::instance().set<bool>("/recording/showClickZoom", state.recordSetup.showClickZoom);
            break;
        }

        case DropdownType::ShapeMode: {
            const MarkupTool nextTool = (id == 1) ? MarkupTool::Spotlight : MarkupTool::Rectangle;
            state.currentTool = nextTool;
            state.isMarkupToolActive = true;
            if (state.activeElement && state.activeElement->tool != nextTool) {
                state.activeElement = nullptr;
            }
            state.sliderPopup.type = SliderPopupType::None;
            state.openSubmenu = SubmenuType::None;
            state.loupeToastMessage = (id == 1)
                ? (isZh ? L"聚光灯模式 (Shift+0)" : L"Spotlight Mode (Shift+0)")
                : (isZh ? L"几何图形模式 (Shift+1)" : L"Shapes Mode (Shift+1)");
            break;
        }

        case DropdownType::NumberFormat: {
            const int clamped = std::clamp(id, 0, static_cast<int>(NumberFormat::COUNT) - 1);
            const NumberFormat fmt = static_cast<NumberFormat>(clamped);
            state.currentNumberFormat = fmt;
            // 切换序号制式用于开启/续接对应制式的独立编号序列，自动解除上一枚落笔序号的激活态，防止误篡改已标好的旧序号
            if (state.activeElement && state.activeElement->tool == MarkupTool::Number) {
                state.activeElement->isActive = false;
                state.activeElement = nullptr;
            }
            tools3000::core::ConfigManager::instance().set<int>("/capture/markup/numberFormat", clamped);
            const std::wstring nextLabel = formatNumberBadgeLabel(state.markup.currentNumber(fmt), fmt);
            state.loupeToastMessage = isZh
                ? std::format(L"序号样式: {} (下一项: {})", numberFormatMenuLabel(fmt, true), nextLabel)
                : std::format(L"Number Style: {} (Next: {})", numberFormatMenuLabel(fmt, false), nextLabel);
            break;
        }

        default:
            break;
    }

    state.dropdownMenu.type = DropdownType::None;
    state.dropdownMenu.items.clear();
    state.toolbarLayoutValid = false;
}

void updateDropdownHover(CaptureState& state, int hoverIndex) {
    if (hoverIndex == state.dropdownMenu.hoveredIndex) return;
    state.dropdownMenu.hoveredIndex = hoverIndex;

    if (state.dropdownMenu.type == DropdownType::BeautyShellBg) {
        if (hoverIndex >= 0 && hoverIndex < static_cast<int>(state.dropdownMenu.items.size())) {
            state.beautyShell.bgType = static_cast<BeautyBackgroundType>(state.dropdownMenu.items[hoverIndex].id);
        } else {
            state.beautyShell.bgType = state.dropdownMenu.originalBgType;
        }
    }
}

void closeDropdownMenu(CaptureState& state, bool restorePreview) {
    if (restorePreview && state.dropdownMenu.type == DropdownType::BeautyShellBg) {
        state.beautyShell.bgType = state.dropdownMenu.originalBgType;
    }
    state.dropdownMenu.type = DropdownType::None;
    state.dropdownMenu.items.clear();
    state.dropdownMenu.hoveredIndex = -1;
    state.toolbarLayoutValid = false;
}

bool hasAnyCapturePopupOpen(const CaptureState& state) noexcept {
    return state.dropdownMenu.type != DropdownType::None ||
           state.sliderPopup.type != SliderPopupType::None ||
           state.openSubmenu != SubmenuType::None ||
           state.isSizeMenuOpen;
}

void closeAllCapturePopups(CaptureState& state, bool restoreDropdownPreview) {
    if (state.dropdownMenu.type != DropdownType::None) {
        closeDropdownMenu(state, restoreDropdownPreview);
    }
    state.sliderPopup.type = SliderPopupType::None;
    state.sliderPopup.isDragging = false;
    state.openSubmenu = SubmenuType::None;
    state.isSizeMenuOpen = false;
}

}  // namespace tools3000::capture

// ─────────────────────────────────────────────────────────────────────────────
// PinContextMenuModel.cpp — 贴图微晶自绘右键复合菜单纯数据模型与多屏布局计算实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinContextMenuModel.h"
#include "core/i18n/UiI18n.h"
#include "capture/PinGroupManager.h"
#include "capture/PinInputController.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace tools3000::capture {

namespace {

PinMenuItem makeSeparator() {
    PinMenuItem sep;
    sep.isSeparator = true;
    return sep;
}

float fallbackMeasureText(const std::wstring& text, float fontPx) noexcept {
    float width = 0.0f;
    for (wchar_t ch : text) {
        width += (ch <= 0x007F) ? (fontPx * 0.58f) : (fontPx * 1.02f);
    }
    return std::ceil(width);
}

std::vector<PinMenuItem> buildGroupSubmenuItems(const PinContextMenuSnapshot& snap) {
    const bool zh = snap.isChinese;
    std::vector<PinMenuItem> sub;
    sub.reserve(24);

    for (const auto& g : snap.groups) {
        if (!g.showInAssignList) continue;
        PinMenuItem item;
        item.hasColorDot = true;
        item.dotColor = g.accentColor;
        item.isChecked = (snap.currentGroupId == g.groupId);
        item.label = tools3000::core::trFmtW(L"归入 {0}", L"Assign to {0}", {g.fullLabel});
        if (g.count > 0) {
            const std::wstring cntStr = std::to_wstring(g.count);
            item.statusBadge = zh ? std::format(L"{} 张", g.count) : cntStr;
            if (tools3000::core::UiI18n::getEffectiveLanguage() != tools3000::core::UiLanguage::ZhCN &&
                tools3000::core::UiI18n::getEffectiveLanguage() != tools3000::core::UiLanguage::EnUS) {
                item.statusBadge = cntStr;
            }
        }
        if (g.groupId >= PinGroupConstants::kMinGroupId &&
            g.groupId <= PinGroupConstants::kMaxShortcutGroupId) {
            item.shortcutText = std::format(L"Ctrl+{}", g.groupId);
        }
        item.command = {PinMenuAction::GroupAssign, g.groupId};
        sub.push_back(std::move(item));
    }

    if (snap.nextAvailableGroupId >= PinGroupConstants::kMinGroupId &&
        snap.nextAvailableGroupId <= PinGroupConstants::kMaxGroupId) {
        PinMenuItem createItem;
        createItem.icon = CaptureIconId::ActionPinGroup;
        const std::wstring nextIdStr = std::to_wstring(snap.nextAvailableGroupId);
        std::wstring nextLabel = tools3000::core::trFmtW(L"{0}组", L"Group {0}", {nextIdStr});
        for (const auto& g : snap.groups) {
            if (g.groupId == snap.nextAvailableGroupId && !g.fullLabel.empty()) {
                nextLabel = g.fullLabel;
                break;
            }
        }
        createItem.label = tools3000::core::trFmtW(L"新建分组并归入 ({0})", L"New Group & Assign ({0})", {nextLabel});
        createItem.command = {PinMenuAction::GroupCreateAndAssign, snap.nextAvailableGroupId};
        sub.push_back(std::move(createItem));
    }

    if (snap.currentGroupId >= PinGroupConstants::kMinGroupId) {
        PinMenuItem removeItem;
        removeItem.icon = CaptureIconId::ActionCancel;
        removeItem.label = tools3000::core::trWCstrFor(zh, L"移出当前分组", L"Remove from Group");
        removeItem.shortcutText = L"Ctrl+0";
        removeItem.command = {PinMenuAction::GroupRemoveCurrent, 0};
        sub.push_back(std::move(removeItem));
    }

    if (snap.ungroupedPinCount >= 1) {
        PinMenuItem autoItem;
        autoItem.icon = CaptureIconId::ActionPinGroup;
        autoItem.label = tools3000::core::trWCstrFor(zh, L"一键编组未分组贴图", L"Auto-Group Ungrouped");
        autoItem.statusBadge = zh
            ? std::format(L"{} 张", snap.ungroupedPinCount)
            : std::format(L"{}", snap.ungroupedPinCount);
        autoItem.shortcutText = L"Ctrl+Shift+G";
        autoItem.command = {PinMenuAction::GroupAutoGroupUngrouped, 0};
        sub.push_back(std::move(autoItem));
    }

    sub.push_back(makeSeparator());

    {
        PinMenuItem viewAll;
        viewAll.icon = CaptureIconId::ActionArrangeGrid;
        viewAll.isChecked = (snap.activeGroupFilter == PinGroupConstants::kFilterAllGroups);
        viewAll.label = tools3000::core::trWCstrFor(zh, L"按组查看：全部贴图", L"View: All Pins");
        viewAll.shortcutText = L"Alt+0";
        viewAll.command = {PinMenuAction::GroupViewAll, 0};
        sub.push_back(std::move(viewAll));
    }

    for (const auto& g : snap.groups) {
        if (!g.showInViewList) continue;
        PinMenuItem viewItem;
        viewItem.hasColorDot = true;
        viewItem.dotColor = g.accentColor;
        viewItem.isChecked = (snap.activeGroupFilter == g.groupId);
        viewItem.label = tools3000::core::trFmtW(L"仅查看 {0}", L"View Only {0}", {g.shortLabel});
        viewItem.statusBadge = (tools3000::core::UiI18n::getEffectiveLanguage() == tools3000::core::UiLanguage::ZhCN && zh)
            ? std::format(L"{} 张", g.count)
            : std::to_wstring(g.count);
        if (g.groupId >= PinGroupConstants::kMinGroupId &&
            g.groupId <= PinGroupConstants::kMaxShortcutGroupId) {
            viewItem.shortcutText = std::format(L"Alt+{}", g.groupId);
        }
        viewItem.command = {PinMenuAction::GroupViewOnly, g.groupId};
        sub.push_back(std::move(viewItem));
    }

    if (snap.currentGroupId >= PinGroupConstants::kMinGroupId) {
        sub.push_back(makeSeparator());

        PinMenuItem arrangeGrp;
        arrangeGrp.icon = CaptureIconId::ActionArrangeGrid;
        arrangeGrp.label = tools3000::core::trWCstrFor(zh, L"网格整理本组贴图", L"Arrange Group in Grid");
        arrangeGrp.command = {PinMenuAction::GroupArrangeCurrent, snap.currentGroupId};
        sub.push_back(std::move(arrangeGrp));

        PinMenuItem renameGrp;
        renameGrp.icon = CaptureIconId::ToolPen;
        renameGrp.label = tools3000::core::trWCstrFor(zh, L"重命名本组...", L"Rename Group...");
        renameGrp.command = {PinMenuAction::GroupRenameCurrent, snap.currentGroupId};
        sub.push_back(std::move(renameGrp));

        if (!snap.currentGroupColorPresets.empty()) {
            PinMenuItem colorParent;
            colorParent.hasColorDot = true;
            colorParent.dotColor = snap.currentGroupAccentColor;
            colorParent.label = tools3000::core::trWCstrFor(zh, L"切换本组颜色", L"Set Group Color");
            for (const auto& preset : snap.currentGroupColorPresets) {
                PinMenuItem cItem;
                cItem.hasColorDot = true;
                cItem.dotColor = preset.color;
                cItem.isChecked = preset.isCurrent;
                cItem.label = preset.name;
                cItem.command = {PinMenuAction::GroupSetColorPreset, preset.slotIndex};
                colorParent.submenu.push_back(std::move(cItem));
            }
            sub.push_back(std::move(colorParent));
        }

        if (snap.currentGroupHasCustomization) {
            PinMenuItem resetMeta;
            resetMeta.icon = CaptureIconId::ActionResetSelection;
            resetMeta.label = tools3000::core::trWCstrFor(zh, L"恢复默认组名与颜色", L"Reset Name & Color");
            resetMeta.command = {PinMenuAction::GroupResetMeta, snap.currentGroupId};
            sub.push_back(std::move(resetMeta));
        }

        sub.push_back(makeSeparator());

        PinMenuItem dissolveGrp;
        dissolveGrp.icon = CaptureIconId::ActionPinGroup;
        dissolveGrp.label = tools3000::core::trWCstrFor(zh, L"解散本组 (保留贴图)", L"Ungroup (Keep Pins)");
        dissolveGrp.command = {PinMenuAction::GroupDissolveCurrent, snap.currentGroupId};
        sub.push_back(std::move(dissolveGrp));

        PinMenuItem closeGrp;
        closeGrp.icon = CaptureIconId::ActionCancel;
        closeGrp.isDanger = true;
        closeGrp.label = tools3000::core::trWCstrFor(zh, L"关闭本组全部贴图", L"Close Group Pins");
        closeGrp.command = {PinMenuAction::GroupCloseCurrent, snap.currentGroupId};
        sub.push_back(std::move(closeGrp));
    }

    return sub;
}

} // namespace

PinContextMenuData PinContextMenuBuilder::buildRootMenu(const PinContextMenuSnapshot& snap) {
    const bool zh = snap.isChinese;
    PinContextMenuData data;
    data.hasHeaderBar = true;
    data.hasOpacitySlider = true;
    data.opacityLabel = tools3000::core::trWCstrFor(zh, L"不透明度", L"Opacity");
    data.initialOpacity = std::clamp(
        snap.opacity, PinInteractionConstants::kMinPinOpacity, PinInteractionConstants::kMaxPinOpacity);

    // 1. 顶部 6 枚高频纯矢量快捷图文格（肌肉记忆恒定锚点）
    const PinMenuAction primaryCopyAction =
        snap.hasPinnedSourceText ? PinMenuAction::CopyNoteText : PinMenuAction::CopyImage;
    data.headerButtons[0] = {
        CaptureIconId::ActionCopy,
        tools3000::core::trWCstrFor(zh, L"复制", L"Copy"),
        {primaryCopyAction, 0},
        {primaryCopyAction, 0},
        false
    };
    data.headerButtons[1] = {
        CaptureIconId::ToolPen,
        tools3000::core::trWCstrFor(zh, L"标注", L"Edit"),
        {PinMenuAction::EditMarkup, 0},
        {PinMenuAction::EditMarkup, 0},
        false
    };
    data.headerButtons[2] = {
        CaptureIconId::ActionExtractText,
        tools3000::core::trWCstrFor(zh, L"识字", L"OCR"),
        {PinMenuAction::ExtractTextDialog, 0},
        {PinMenuAction::ExtractTextDialog, 0},
        false
    };
    data.headerButtons[3] = {
        CaptureIconId::ActionRotate,
        tools3000::core::trWCstrFor(zh, L"旋转", L"Rotate"),
        {PinMenuAction::RotateRight90, 0},
        {PinMenuAction::RotateLeft90, 0}, // Shift+点击直接逆时针旋转 90°
        false
    };
    data.headerButtons[4] = {
        CaptureIconId::ActionInvert,
        tools3000::core::trWCstrFor(zh, L"反色", L"Invert"),
        {PinMenuAction::ToggleInvert, 0},
        {PinMenuAction::ToggleInvert, 0},
        snap.isInverted
    };
    data.headerButtons[5] = {
        CaptureIconId::ActionSave,
        tools3000::core::trWCstrFor(zh, L"保存", L"Save"),
        {PinMenuAction::SaveImage, 0},
        {PinMenuAction::SaveImage, 0},
        false
    };

    // 2. 动态上下文置顶：若当前已在贴图上划选了局部 OCR 文字块，首行直接浮现「复制所选文字」
    if (snap.hasSelectedText) {
        PinMenuItem copySel;
        copySel.icon = CaptureIconId::ActionCopy;
        copySel.label = tools3000::core::trWCstrFor(zh, L"复制所选文字", L"Copy Selected Text");
        copySel.shortcutText = L"Ctrl+C";
        copySel.command = {PinMenuAction::CopySelectedText, 0};
        data.items.push_back(std::move(copySel));
        data.items.push_back(makeSeparator());
    }

    // 3. 窗口核心状态直达项（折叠 & 穿透）
    {
        PinMenuItem foldItem;
        foldItem.icon = CaptureIconId::ActionFold;
        foldItem.isChecked = snap.isFolded;
        foldItem.label = snap.isFolded
            ? (tools3000::core::trWCstrFor(zh, L"展开迷你贴图", L"Expand Pin"))
            : (tools3000::core::trWCstrFor(zh, L"折叠为迷你条", L"Collapse to Bar"));
        foldItem.shortcutText = L"M";
        foldItem.command = {PinMenuAction::ToggleFold, 0};
        data.items.push_back(std::move(foldItem));
    }
    {
        PinMenuItem ghostItem;
        ghostItem.icon = CaptureIconId::ActionClickThrough;
        ghostItem.isChecked = snap.isClickThrough;
        ghostItem.label = tools3000::core::trWCstrFor(zh, L"鼠标穿透 (幽灵模式)", L"Click-Through (Ghost)");
        ghostItem.shortcutText = L"T";
        ghostItem.command = {PinMenuAction::ToggleClickThrough, 0};
        data.items.push_back(std::move(ghostItem));
    }

    data.items.push_back(makeSeparator());

    // 4. 二级聚合：文字划选与复制（消除 4 个 OCR 动作在根菜单并列打架）
    {
        PinMenuItem ocrParent;
        ocrParent.icon = CaptureIconId::ActionExtractText;
        ocrParent.label = tools3000::core::trWCstrFor(zh, L"文字划选与翻译", L"Text, OCR & Translate");
        if (snap.isTranslationInFlight) {
            ocrParent.statusBadge = tools3000::core::trWCstrFor(zh, L"翻译中", L"Translating");
        } else if (snap.isShowingTranslation) {
            ocrParent.statusBadge = tools3000::core::trWCstrFor(zh, L"译文中", L"Translated");
        } else if (snap.isTextSelectionMode) {
            ocrParent.statusBadge = tools3000::core::trWCstrFor(zh, L"划选中", L"Active");
        }

        PinMenuItem openOcr;
        openOcr.icon = CaptureIconId::ActionExtractText;
        openOcr.label = tools3000::core::trWCstrFor(zh, L"提取全文并复制 (OCR)", L"Extract & Copy Text (OCR)");
        openOcr.shortcutText = L"Ctrl+T";
        openOcr.command = {PinMenuAction::ExtractTextDialog, 0};
        ocrParent.submenu.push_back(std::move(openOcr));

        PinMenuItem translateItem;
        translateItem.icon = CaptureIconId::ActionTranslate;
        translateItem.label = snap.isTranslationInFlight
            ? tools3000::core::trWCstrFor(zh, L"正在原位翻译...", L"Translating...")
            : (snap.hasSelectedText
                ? tools3000::core::trWCstrFor(zh, L"原位翻译所选文字", L"Translate Selected In-Place")
                : tools3000::core::trWCstrFor(zh, L"原位翻译图片", L"Translate Image In-Place"));
        translateItem.shortcutText = L"Ctrl+Shift+T";
        translateItem.command = {PinMenuAction::TranslateInPlace, 0};
        ocrParent.submenu.push_back(std::move(translateItem));

        if (snap.hasTranslation) {
            PinMenuItem toggleTr;
            toggleTr.icon = CaptureIconId::ActionTranslate;
            toggleTr.isChecked = snap.isShowingTranslation;
            toggleTr.label = snap.isShowingTranslation
                ? tools3000::core::trWCstrFor(zh, L"切换查看原图对照", L"Show Original Image")
                : tools3000::core::trWCstrFor(zh, L"切换显示译图", L"Show Translated Image");
            toggleTr.shortcutText = L"Tab";
            toggleTr.command = {PinMenuAction::ToggleTranslationOverlay, 0};
            ocrParent.submenu.push_back(std::move(toggleTr));

            PinMenuItem copyTr;
            copyTr.icon = CaptureIconId::ActionCopy;
            copyTr.label = tools3000::core::trWCstrFor(zh, L"复制全部译文", L"Copy Translated Text");
            copyTr.command = {PinMenuAction::CopyTranslatedText, 0};
            ocrParent.submenu.push_back(std::move(copyTr));

            PinMenuItem reTr;
            reTr.icon = CaptureIconId::ActionTranslate;
            reTr.label = tools3000::core::trWCstrFor(zh, L"重新翻译", L"Retranslate");
            reTr.command = {PinMenuAction::RetranslateInPlace, 0};
            ocrParent.submenu.push_back(std::move(reTr));
        }

        PinMenuItem copyAllTxt;
        copyAllTxt.icon = CaptureIconId::ActionCopy;
        copyAllTxt.label = tools3000::core::trWCstrFor(zh, L"快速复制已识文字", L"Quick Copy Recognized Text");
        copyAllTxt.shortcutText = L"Ctrl+Shift+C";
        copyAllTxt.command = {PinMenuAction::CopyAllText, 0};
        ocrParent.submenu.push_back(std::move(copyAllTxt));

        if (snap.hasPinnedSourceText) {
            PinMenuItem copyImg;
            copyImg.icon = CaptureIconId::ActionSnapshot;
            copyImg.label = tools3000::core::trWCstrFor(zh, L"复制卡片为图片", L"Copy Card as Image");
            copyImg.command = {PinMenuAction::CopyImage, 0};
            ocrParent.submenu.push_back(std::move(copyImg));
        }

        ocrParent.submenu.push_back(makeSeparator());

        PinMenuItem selMode;
        selMode.icon = CaptureIconId::ToolText;
        selMode.isChecked = snap.isTextSelectionMode;
        selMode.label = tools3000::core::trWCstrFor(zh, L"鼠标划选文字模式", L"Text Selection Mode");
        selMode.shortcutText = L"Alt";
        selMode.command = {PinMenuAction::ToggleTextSelectionMode, 0};
        ocrParent.submenu.push_back(std::move(selMode));

        PinMenuItem selAll;
        selAll.icon = CaptureIconId::ActionInvertSelection;
        selAll.label = tools3000::core::trWCstrFor(zh, L"全选贴图文字", L"Select All Text");
        selAll.shortcutText = L"Ctrl+A";
        selAll.command = {PinMenuAction::SelectAllText, 0};
        ocrParent.submenu.push_back(std::move(selAll));

        ocrParent.submenu.push_back(makeSeparator());

        // 极客直切入口：贴图级「识字语种」覆写（默认遵循自动嗅探，1% 极端短词/生僻字场景可零切换重识并自动重译）
        {
            const std::string curScript = snap.activeOcrScriptMode.empty() ? "auto" : snap.activeOcrScriptMode;
            PinMenuItem scriptParent;
            scriptParent.icon = CaptureIconId::ActionTranslate;
            scriptParent.label = tools3000::core::trWCstrFor(zh, L"识字语种", L"OCR Script");
            if (curScript == "cjk") {
                scriptParent.statusBadge = snap.engineIsV6
                    ? tools3000::core::trWCstrFor(zh, L"通用", L"50+")
                    : tools3000::core::trWCstrFor(zh, L"中英", L"CJK");
            } else if (curScript == "japan") {
                scriptParent.statusBadge = tools3000::core::trWCstrFor(zh, L"日语", L"JA");
            } else if (curScript == "korean") {
                scriptParent.statusBadge = tools3000::core::trWCstrFor(zh, L"韩语", L"KO");
            } else if (curScript == "cyrillic") {
                scriptParent.statusBadge = tools3000::core::trWCstrFor(zh, L"俄语", L"RU");
            } else if (curScript == "latin") {
                scriptParent.statusBadge = tools3000::core::trWCstrFor(zh, L"西/葡", L"LA");
            } else if (curScript == "arabic") {
                scriptParent.statusBadge = tools3000::core::trWCstrFor(zh, L"阿语", L"AR");
            } else if (curScript == "thai") {
                scriptParent.statusBadge = tools3000::core::trWCstrFor(zh, L"泰语", L"TH");
            }

            auto makeScriptItem = [&](int param, const char* modeKey,
                                      const wchar_t* zhLabel, const wchar_t* enLabel,
                                      bool installed) {
                PinMenuItem item;
                item.icon = CaptureIconId::ActionTranslate;
                item.isChecked = (curScript == modeKey);
                item.label = tools3000::core::trWCstrFor(zh, zhLabel, enLabel);
                if (!installed) {
                    item.statusBadge = tools3000::core::trWCstrFor(zh, L"未安装", L"Not installed");
                }
                item.command = {PinMenuAction::SetOcrScriptMode, param};
                scriptParent.submenu.push_back(std::move(item));
            };

            makeScriptItem(PinOcrScriptParam::kAuto, "auto", L"自动识别 (推荐)", L"Auto Detect (Recommended)", true);
            scriptParent.submenu.push_back(makeSeparator());
            if (snap.engineIsV6) {
                makeScriptItem(PinOcrScriptParam::kCjk, "cjk", L"中 / 英 / 日 / 欧文 (50 语种)", L"CJK / EN / JA / European (50+)", true);
                makeScriptItem(PinOcrScriptParam::kKorean, "korean", L"韩语", L"Korean", snap.scriptPackKoreanInstalled);
                makeScriptItem(PinOcrScriptParam::kCyrillic, "cyrillic", L"俄语 (西里尔)", L"Russian (Cyrillic)", snap.scriptPackCyrillicInstalled);
                makeScriptItem(PinOcrScriptParam::kArabic, "arabic", L"阿拉伯语 / 波斯语", L"Arabic / Persian", snap.scriptPackArabicInstalled);
                makeScriptItem(PinOcrScriptParam::kThai, "thai", L"泰语", L"Thai", snap.scriptPackThaiInstalled);
            } else {
                makeScriptItem(PinOcrScriptParam::kCjk, "cjk", L"中文与英文", L"Chinese & English", true);
                makeScriptItem(PinOcrScriptParam::kJapan, "japan", L"日语", L"Japanese", snap.scriptPackJapanInstalled);
                makeScriptItem(PinOcrScriptParam::kKorean, "korean", L"韩语", L"Korean", snap.scriptPackKoreanInstalled);
                makeScriptItem(PinOcrScriptParam::kCyrillic, "cyrillic", L"俄语 (西里尔)", L"Russian (Cyrillic)", snap.scriptPackCyrillicInstalled);
                makeScriptItem(PinOcrScriptParam::kLatin, "latin", L"西 / 葡 / 欧文 (拉丁)", L"Spanish / Portuguese (Latin)", snap.scriptPackLatinInstalled);
            }

            ocrParent.submenu.push_back(std::move(scriptParent));
        }

        data.items.push_back(std::move(ocrParent));
    }

    // 5. 二级聚合：旋转翻转与滤镜（异常缩放/滤镜状态在一级微标直观回显）
    {
        PinMenuItem transformParent;
        transformParent.icon = CaptureIconId::ActionRotate;
        transformParent.label = tools3000::core::trWCstrFor(zh, L"旋转翻转与滤镜", L"Transform & Filters");

        const int scalePct = static_cast<int>(std::lround(snap.scale * 100.0f));
        std::wstring badge;
        if (std::abs(snap.scale - 1.0f) >= 0.015f) {
            badge = std::format(L"{}%", scalePct);
        }
        if (snap.isGrayscale) {
            if (!badge.empty()) badge += L" · ";
            badge += tools3000::core::trWCstrFor(zh, L"灰度", L"Gray");
        } else if (snap.isInverted) {
            if (!badge.empty()) badge += L" · ";
            badge += tools3000::core::trWCstrFor(zh, L"反色", L"Inv");
        }
        transformParent.statusBadge = std::move(badge);

        PinMenuItem reset1x;
        reset1x.icon = CaptureIconId::ActionResetScale;
        reset1x.isChecked = (std::abs(snap.scale - 1.0f) < 0.015f);
        reset1x.label = tools3000::core::trWCstrFor(zh, L"1:1 原始尺寸", L"1:1 Original Scale");
        if (std::abs(snap.scale - 1.0f) >= 0.015f) {
            reset1x.statusBadge = std::format(L"{}%", scalePct);
        }
        reset1x.shortcutText = L"0";
        reset1x.command = {PinMenuAction::ResetScale, 0};
        transformParent.submenu.push_back(std::move(reset1x));

        transformParent.submenu.push_back(makeSeparator());

        PinMenuItem rotCw;
        rotCw.icon = CaptureIconId::ActionRotate;
        rotCw.label = tools3000::core::trWCstrFor(zh, L"顺时针旋转 90°", L"Rotate Right 90°");
        rotCw.shortcutText = L"R";
        rotCw.command = {PinMenuAction::RotateRight90, 0};
        transformParent.submenu.push_back(std::move(rotCw));

        PinMenuItem rotCcw;
        rotCcw.icon = CaptureIconId::ActionRotateLeft;
        rotCcw.label = tools3000::core::trWCstrFor(zh, L"逆时针旋转 90°", L"Rotate Left 90°");
        rotCcw.shortcutText = L"L";
        rotCcw.command = {PinMenuAction::RotateLeft90, 0};
        transformParent.submenu.push_back(std::move(rotCcw));

        PinMenuItem flipH;
        flipH.icon = CaptureIconId::ActionFlipHorizontal;
        flipH.label = tools3000::core::trWCstrFor(zh, L"水平翻转", L"Flip Horizontal");
        flipH.shortcutText = L"H";
        flipH.command = {PinMenuAction::FlipHorizontal, 0};
        transformParent.submenu.push_back(std::move(flipH));

        PinMenuItem flipV;
        flipV.icon = CaptureIconId::ActionFlipVertical;
        flipV.label = tools3000::core::trWCstrFor(zh, L"垂直翻转", L"Flip Vertical");
        flipV.shortcutText = L"V";
        flipV.command = {PinMenuAction::FlipVertical, 0};
        transformParent.submenu.push_back(std::move(flipV));

        transformParent.submenu.push_back(makeSeparator());

        PinMenuItem invItem;
        invItem.icon = CaptureIconId::ActionInvert;
        invItem.isChecked = snap.isInverted;
        invItem.label = tools3000::core::trWCstrFor(zh, L"色彩反转", L"Invert Colors");
        invItem.shortcutText = L"I";
        invItem.command = {PinMenuAction::ToggleInvert, 0};
        transformParent.submenu.push_back(std::move(invItem));

        PinMenuItem grayItem;
        grayItem.icon = CaptureIconId::ActionGrayscale;
        grayItem.isChecked = snap.isGrayscale;
        grayItem.label = tools3000::core::trWCstrFor(zh, L"灰度黑白", L"Grayscale");
        grayItem.shortcutText = L"G";
        grayItem.command = {PinMenuAction::ToggleGrayscale, 0};
        transformParent.submenu.push_back(std::move(grayItem));

        data.items.push_back(std::move(transformParent));
    }

    // 6. 二级聚合：贴图分组（已分组时直接展示宝石色圆点 + 组标胶囊）
    {
        PinMenuItem groupParent;
        if (snap.currentGroupId >= PinGroupConstants::kMinGroupId) {
            groupParent.hasColorDot = true;
            groupParent.dotColor = snap.currentGroupAccentColor;
            groupParent.label = tools3000::core::trWCstrFor(zh, L"贴图分组", L"Pin Group");
            groupParent.statusBadge = snap.currentGroupShortLabel;
        } else {
            groupParent.icon = CaptureIconId::ActionPinGroup;
            groupParent.label = tools3000::core::trWCstrFor(zh, L"贴图分组", L"Pin Group");
        }
        groupParent.shortcutText = L"Ctrl+1~9";
        groupParent.submenu = buildGroupSubmenuItems(snap);
        data.items.push_back(std::move(groupParent));
    }

    // 7. 场景感知按需显隐：仅当存在多张贴图 (>= 2) 或全局处于隐藏态时才展示批量管理
    if (snap.totalPinCount >= 2 || snap.allPinsHidden) {
        PinMenuItem multiParent;
        multiParent.icon = CaptureIconId::ActionArrangeGrid;
        multiParent.label = tools3000::core::trWCstrFor(zh, L"多贴图批量管理", L"Manage All Pins");
        multiParent.statusBadge = zh
            ? std::format(L"{} 张", snap.totalPinCount)
            : std::format(L"{}", snap.totalPinCount);

        PinMenuItem arrangeAll;
        arrangeAll.icon = CaptureIconId::ActionArrangeGrid;
        arrangeAll.label = tools3000::core::trWCstrFor(zh, L"网格整理全部贴图", L"Arrange All Pins");
        arrangeAll.command = {PinMenuAction::ArrangeAllPins, 0};
        multiParent.submenu.push_back(std::move(arrangeAll));

        PinMenuItem hideAll;
        hideAll.icon = CaptureIconId::ActionClickThrough;
        hideAll.isChecked = snap.allPinsHidden;
        hideAll.label = snap.allPinsHidden
            ? (tools3000::core::trWCstrFor(zh, L"显示全部贴图", L"Show All Pins"))
            : (tools3000::core::trWCstrFor(zh, L"隐藏全部贴图", L"Hide All Pins"));
        hideAll.shortcutText = L"Ctrl+Alt+Shift+H";
        hideAll.command = {PinMenuAction::ToggleHideAllPins, 0};
        multiParent.submenu.push_back(std::move(hideAll));

        multiParent.submenu.push_back(makeSeparator());

        if (snap.totalPinCount >= 2) {
            PinMenuItem closeOthers;
            closeOthers.icon = CaptureIconId::ActionCancel;
            closeOthers.label = tools3000::core::trWCstrFor(zh, L"关闭其他贴图", L"Close Other Pins");
            closeOthers.command = {PinMenuAction::CloseOtherPins, 0};
            multiParent.submenu.push_back(std::move(closeOthers));
        }

        PinMenuItem closeAll;
        closeAll.icon = CaptureIconId::ActionCancel;
        closeAll.isDanger = true;
        closeAll.label = tools3000::core::trWCstrFor(zh, L"关闭全部贴图", L"Close All Pins");
        closeAll.command = {PinMenuAction::CloseAllPins, 0};
        multiParent.submenu.push_back(std::move(closeAll));

        data.items.push_back(std::move(multiParent));
    }

    data.items.push_back(makeSeparator());

    // 8. 底部干净利落的单张关闭出口
    {
        PinMenuItem closeItem;
        closeItem.icon = CaptureIconId::ActionCancel;
        closeItem.isDanger = true;
        closeItem.label = tools3000::core::trWCstrFor(zh, L"关闭贴图", L"Close Pin");
        closeItem.shortcutText = L"Esc";
        closeItem.command = {PinMenuAction::ClosePin, 0};
        data.items.push_back(std::move(closeItem));
    }

    return data;
}

PinContextMenuData PinContextMenuBuilder::buildGroupOnlyMenu(const PinContextMenuSnapshot& snap) {
    PinContextMenuData data;
    data.hasHeaderBar = false;
    data.hasOpacitySlider = false;
    data.initialOpacity = snap.opacity;
    data.items = buildGroupSubmenuItems(snap);
    return data;
}

PinMenuCommand PinContextMenuBuilder::resolveDirectKeyShortcut(WPARAM vk,
                                                               bool ctrl,
                                                               bool shift,
                                                               bool alt,
                                                               const PinContextMenuSnapshot& snap) noexcept {
    if (ctrl && !alt) {
        if (vk == 'C') {
            if (shift) return {PinMenuAction::CopyAllText, 0};
            if (snap.hasSelectedText) return {PinMenuAction::CopySelectedText, 0};
            if (snap.hasPinnedSourceText) return {PinMenuAction::CopyNoteText, 0};
            return {PinMenuAction::CopyImage, 0};
        }
        if (vk == 'S' && !shift) return {PinMenuAction::SaveImage, 0};
        if (vk == 'T' && shift) return {PinMenuAction::TranslateInPlace, 0};
        if (vk == 'T' && !shift) return {PinMenuAction::ExtractTextDialog, 0};
        if (vk == 'A' && !shift) return {PinMenuAction::SelectAllText, 0};
        if (vk == 'W' && !shift) return {PinMenuAction::ClosePin, 0};
        if (vk == 'G' && shift)  return {PinMenuAction::GroupAutoGroupUngrouped, 0};
        if (vk == '0' && !shift) return {PinMenuAction::GroupRemoveCurrent, 0};
        if (vk >= '1' && vk <= '9' && !shift) {
            return {PinMenuAction::GroupAssign, static_cast<int>(vk - '0')};
        }
        return {};
    }

    if (alt && !ctrl && !shift) {
        if (vk == '0') return {PinMenuAction::GroupViewAll, 0};
        if (vk >= '1' && vk <= '9') {
            return {PinMenuAction::GroupViewOnly, static_cast<int>(vk - '0')};
        }
        return {};
    }

    if (!ctrl && !alt) {
        switch (vk) {
            case 'E': return {PinMenuAction::EditMarkup, 0};
            case 'M': return {PinMenuAction::ToggleFold, 0};
            case 'T':
            case VK_F4: return {PinMenuAction::ToggleClickThrough, 0};
            case 'I': return {PinMenuAction::ToggleInvert, 0};
            case 'G': return {PinMenuAction::ToggleGrayscale, 0};
            case 'R': return shift ? PinMenuCommand{PinMenuAction::RotateLeft90, 0}
                                   : PinMenuCommand{PinMenuAction::RotateRight90, 0};
            case 'L':
            case '1': return {PinMenuAction::RotateLeft90, 0};
            case '2': return {PinMenuAction::RotateRight90, 0};
            case 'H':
            case '3': return {PinMenuAction::FlipHorizontal, 0};
            case 'V':
            case '4': return {PinMenuAction::FlipVertical, 0};
            case '0': return {PinMenuAction::ResetScale, 0};
            case VK_TAB: return snap.hasTranslation
                ? PinMenuCommand{PinMenuAction::ToggleTranslationOverlay, 0}
                : PinMenuCommand{};
            case VK_DELETE: return {PinMenuAction::ClosePin, 0};
            default: break;
        }
    }
    return {};
}

PinMenuPanelLayout PinContextMenuLayoutEngine::computePanelLayout(
    bool hasHeaderBar,
    const std::array<PinMenuHeaderButton, PinContextMenuMetrics::kHeaderActionCount>* headerButtons,
    bool hasOpacitySlider,
    const std::wstring& opacityLabel,
    const std::vector<PinMenuItem>& items,
    float dpiScale,
    float maxWorkAreaHeightPx,
    const PinMenuTextMeasureFn& measureText) {

    const float s = std::clamp(dpiScale > 0.0f ? dpiScale : 1.0f, 1.0f, 4.0f);
    const auto measure = [&](const std::wstring& txt, float fontDip) -> float {
        if (txt.empty()) return 0.0f;
        const float px = (std::max)(PinContextMenuMetrics::kMinUiFontPx, fontDip * s);
        return measureText ? measureText(txt, px) : fallbackMeasureText(txt, px);
    };

    const float shadow = std::ceil(PinContextMenuMetrics::kShadowMarginDip * s);
    const float padX = std::round(PinContextMenuMetrics::kPanelPadXDip * s);
    const float padY = std::round(PinContextMenuMetrics::kPanelPadYDip * s);

    float minPanelW = (hasHeaderBar ? PinContextMenuMetrics::kMinRootPanelWidthDip
                                    : PinContextMenuMetrics::kMinSubPanelWidthDip) * s;
    const float maxPanelW = PinContextMenuMetrics::kMaxPanelWidthDip * s;

    if (hasHeaderBar && headerButtons) {
        float maxBtnLabelW = 0.0f;
        for (const auto& b : *headerButtons) {
            maxBtnLabelW = (std::max)(maxBtnLabelW, measure(b.shortLabel, PinContextMenuMetrics::kHeaderLabelFontDip));
        }
        const float neededBtnW = maxBtnLabelW + 10.0f * s;
        const float neededHeaderTotalW =
            padX * 2.0f +
            neededBtnW * static_cast<float>(PinContextMenuMetrics::kHeaderActionCount) +
            PinContextMenuMetrics::kHeaderBtnGapDip * s * static_cast<float>(PinContextMenuMetrics::kHeaderActionCount - 1);
        minPanelW = (std::max)(minPanelW, neededHeaderTotalW);
    }

    float maxItemNeededW = 0.0f;
    for (const auto& item : items) {
        if (item.isSeparator) continue;
        float rowW = padX * 2.0f + PinContextMenuMetrics::kItemIconLeadWidthDip * s +
                     measure(item.label, PinContextMenuMetrics::kItemLabelFontDip) +
                     PinContextMenuMetrics::kItemRightPadDip * s;
        if (!item.statusBadge.empty()) {
            rowW += PinContextMenuMetrics::kItemTextGapDip * s +
                    measure(item.statusBadge, PinContextMenuMetrics::kBadgeFontDip) +
                    PinContextMenuMetrics::kStatusBadgePadXDip * 2.0f * s;
        }
        if (!item.shortcutText.empty()) {
            rowW += 16.0f * s + measure(item.shortcutText, PinContextMenuMetrics::kShortcutFontDip);
        }
        if (item.hasSubmenu()) {
            rowW += PinContextMenuMetrics::kItemChevronWidthDip * s;
        }
        maxItemNeededW = (std::max)(maxItemNeededW, rowW);
    }

    const float panelW = std::ceil(std::clamp((std::max)(minPanelW, maxItemNeededW), minPanelW, maxPanelW));

    PinMenuPanelLayout layout;
    layout.hasHeaderBar = hasHeaderBar;
    layout.hasOpacitySlider = hasOpacitySlider;

    const float panelLeft = shadow;
    const float panelTop = shadow;
    const float panelRight = panelLeft + panelW;
    float curY = panelTop + padY;

    // 1. 顶部 6 枚快捷图文栏
    if (hasHeaderBar) {
        const float gap = std::round(PinContextMenuMetrics::kHeaderBtnGapDip * s);
        const float btnH = std::round(PinContextMenuMetrics::kHeaderBtnHeightDip * s);
        const float availW = panelW - padX * 2.0f - gap * static_cast<float>(PinContextMenuMetrics::kHeaderActionCount - 1);
        const float btnW = availW / static_cast<float>(PinContextMenuMetrics::kHeaderActionCount);
        float bx = panelLeft + padX;
        for (int i = 0; i < PinContextMenuMetrics::kHeaderActionCount; ++i) {
            layout.headerButtonRects[static_cast<size_t>(i)] = D2D1::RectF(bx, curY, bx + btnW, curY + btnH);
            bx += btnW + gap;
        }
        curY += btnH + std::round(PinContextMenuMetrics::kSeparatorHeightDip * s);
    }

    // 2. 不透明度连续滑块行
    if (hasOpacitySlider) {
        const float rowH = std::round(PinContextMenuMetrics::kSliderRowHeightDip * s);
        const float rowLeft = panelLeft + padX;
        const float rowRight = panelRight - padX;
        layout.sliderRowRect = D2D1::RectF(rowLeft, curY, rowRight, curY + rowH);

        const float lblW = measure(opacityLabel, PinContextMenuMetrics::kShortcutFontDip) + 8.0f * s;
        const float pctW = std::round(PinContextMenuMetrics::kSliderPctPillWidthDip * s);
        layout.sliderLabelRect = D2D1::RectF(rowLeft + 6.0f * s, curY, rowLeft + 6.0f * s + lblW, curY + rowH);
        layout.sliderPctPillRect = D2D1::RectF(rowRight - pctW - 2.0f * s, curY + 6.0f * s,
                                                rowRight - 2.0f * s, curY + rowH - 6.0f * s);
        const float trackLeft = layout.sliderLabelRect.right + 8.0f * s;
        const float trackRight = (std::max)(trackLeft + 48.0f * s, layout.sliderPctPillRect.left - 10.0f * s);
        const float trackMidY = curY + rowH * 0.5f;
        const float halfTrackH = PinContextMenuMetrics::kSliderTrackHeightDip * s * 0.5f;
        layout.sliderTrackRect = D2D1::RectF(trackLeft, trackMidY - halfTrackH, trackRight, trackMidY + halfTrackH);

        curY += rowH + std::round(PinContextMenuMetrics::kSeparatorHeightDip * s);
    }

    // 3. 列表区滚动视口与各项矩形
    const float itemsStartY = curY;
    float itemLocalY = 0.0f;
    layout.itemRects.reserve(items.size());
    const float itemH = std::round(PinContextMenuMetrics::kItemRowHeightDip * s);
    const float sepH = std::round(PinContextMenuMetrics::kSeparatorHeightDip * s);

    for (const auto& item : items) {
        const float h = item.isSeparator ? sepH : itemH;
        layout.itemRects.push_back(D2D1::RectF(
            panelLeft + padX,
            itemsStartY + itemLocalY,
            panelRight - padX,
            itemsStartY + itemLocalY + h));
        itemLocalY += h;
    }
    layout.totalItemsContentHeight = itemLocalY;

    const float fixedChromeH = (itemsStartY - panelTop) + padY;
    const float safeMaxWorkH = (maxWorkAreaHeightPx > 140.0f * s)
        ? (maxWorkAreaHeightPx - PinContextMenuMetrics::kScreenEdgeMarginDip * 2.0f * s)
        : (fixedChromeH + itemLocalY + shadow * 2.0f);
    const float maxAllowedViewportH = (std::max)(itemH * 3.0f, safeMaxWorkH - fixedChromeH);
    const float viewportH = (std::min)(itemLocalY, maxAllowedViewportH);

    layout.itemsViewportRect = D2D1::RectF(panelLeft, itemsStartY, panelRight, itemsStartY + viewportH);
    layout.maxScrollOffsetY = (std::max)(0.0f, itemLocalY - viewportH);

    const float panelBottom = itemsStartY + viewportH + padY;
    layout.panelRect = D2D1::RectF(panelLeft, panelTop, panelRight, panelBottom);
    layout.windowSize.cx = static_cast<LONG>(std::ceil(panelRight + shadow));
    layout.windowSize.cy = static_cast<LONG>(std::ceil(panelBottom + shadow));
    return layout;
}

POINT PinContextMenuLayoutEngine::placeRootWindow(POINT cursorScreenPt,
                                                  SIZE windowSize,
                                                  const RECT& workArea,
                                                  float shadowMarginPx) noexcept {
    const int sh = static_cast<int>(std::lround(shadowMarginPx));
    const int edge = 4;
    const int panelW = (std::max)(1, static_cast<int>(windowSize.cx) - sh * 2);
    const int panelH = (std::max)(1, static_cast<int>(windowSize.cy) - sh * 2);

    int panelLeft = cursorScreenPt.x;
    int panelTop = cursorScreenPt.y;

    if (panelLeft + panelW > workArea.right - edge) {
        panelLeft = cursorScreenPt.x - panelW;
    }
    if (panelTop + panelH > workArea.bottom - edge) {
        panelTop = cursorScreenPt.y - panelH;
    }

    const int minLeft = workArea.left + edge;
    const int maxLeft = (std::max)(minLeft, static_cast<int>(workArea.right) - edge - panelW);
    const int minTop = workArea.top + edge;
    const int maxTop = (std::max)(minTop, static_cast<int>(workArea.bottom) - edge - panelH);

    panelLeft = std::clamp(panelLeft, minLeft, maxLeft);
    panelTop = std::clamp(panelTop, minTop, maxTop);

    return POINT{panelLeft - sh, panelTop - sh};
}

POINT PinContextMenuLayoutEngine::placeSubmenuWindow(const RECT& parentPanelScreenRect,
                                                     float anchorItemTopScreenY,
                                                     SIZE childWindowSize,
                                                     const RECT& workArea,
                                                     float shadowMarginPx,
                                                     float overlapPx) noexcept {
    const int sh = static_cast<int>(std::lround(shadowMarginPx));
    const int ov = static_cast<int>(std::lround(overlapPx));
    const int edge = 4;
    const int childPanelW = (std::max)(1, static_cast<int>(childWindowSize.cx) - sh * 2);
    const int childPanelH = (std::max)(1, static_cast<int>(childWindowSize.cy) - sh * 2);

    int childPanelLeft = parentPanelScreenRect.right - ov;
    if (childPanelLeft + childPanelW > workArea.right - edge) {
        const int flipLeft = parentPanelScreenRect.left - childPanelW + ov;
        if (flipLeft >= workArea.left + edge ||
            (workArea.right - parentPanelScreenRect.right) < (parentPanelScreenRect.left - workArea.left)) {
            childPanelLeft = flipLeft;
        }
    }

    const int minLeft = workArea.left + edge;
    const int maxLeft = (std::max)(minLeft, static_cast<int>(workArea.right) - edge - childPanelW);
    childPanelLeft = std::clamp(childPanelLeft, minLeft, maxLeft);

    // 子菜单面板顶部对齐父级触发行顶部（向上偏移一个面板内边距使首行水平齐平）
    int childPanelTop = static_cast<int>(std::lround(anchorItemTopScreenY)) - 6;
    const int minTop = workArea.top + edge;
    const int maxTop = (std::max)(minTop, static_cast<int>(workArea.bottom) - edge - childPanelH);
    childPanelTop = std::clamp(childPanelTop, minTop, maxTop);

    return POINT{childPanelLeft - sh, childPanelTop - sh};
}

float PinContextMenuLayoutEngine::opacityFromSliderX(float clientX,
                                                     const D2D1_RECT_F& trackRect) noexcept {
    const float trackW = trackRect.right - trackRect.left;
    if (trackW <= 1.0f) return PinInteractionConstants::kMaxPinOpacity;

    const float t = std::clamp((clientX - trackRect.left) / trackW, 0.0f, 1.0f);
    float raw = PinInteractionConstants::kMinPinOpacity +
                t * (PinInteractionConstants::kMaxPinOpacity - PinInteractionConstants::kMinPinOpacity);

    // 对常用刻度 (50%, 75%, 100%) 施加柔和磁吸，同时保持全区间 1% 精度连续可调
    for (float snapTarget : {0.50f, 0.75f, 1.00f}) {
        if (std::abs(raw - snapTarget) <= PinContextMenuMetrics::kSliderMagneticSnapTolerance) {
            return snapTarget;
        }
    }
    raw = std::round(raw * 100.0f) / 100.0f;
    return std::clamp(raw, PinInteractionConstants::kMinPinOpacity, PinInteractionConstants::kMaxPinOpacity);
}

float PinContextMenuLayoutEngine::sliderXFromOpacity(float opacity,
                                                     const D2D1_RECT_F& trackRect) noexcept {
    const float clamped = std::clamp(
        opacity, PinInteractionConstants::kMinPinOpacity, PinInteractionConstants::kMaxPinOpacity);
    const float span = PinInteractionConstants::kMaxPinOpacity - PinInteractionConstants::kMinPinOpacity;
    const float t = (span > 0.0001f) ? ((clamped - PinInteractionConstants::kMinPinOpacity) / span) : 1.0f;
    return trackRect.left + t * (trackRect.right - trackRect.left);
}

int PinContextMenuLayoutEngine::findNextSelectableIndex(const std::vector<PinMenuItem>& items,
                                                        int currentIndex,
                                                        int stepDirection) noexcept {
    if (items.empty() || stepDirection == 0) return -1;
    const int n = static_cast<int>(items.size());
    const int step = (stepDirection > 0) ? 1 : -1;
    int probe = currentIndex;
    if (probe < 0 || probe >= n) {
        probe = (step > 0) ? -1 : n;
    }
    for (int count = 0; count < n; ++count) {
        probe = (probe + step + n) % n;
        if (items[static_cast<size_t>(probe)].isSelectable()) {
            return probe;
        }
    }
    return -1;
}

} // namespace tools3000::capture

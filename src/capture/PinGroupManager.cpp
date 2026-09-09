// ─────────────────────────────────────────────────────────────────────────────
// PinGroupManager.cpp — 贴图分组自定义名称与主题色持久化管理引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinGroupManager.h"
#include "core/config/ConfigManager.h"
#include "core/utils/WinUtils.h"

#include <atomic>
#include <cctype>
#include <cwctype>
#include <format>
#include <mutex>

namespace tools3000::capture {

namespace {

constexpr const char* kPinGroupsConfigKey = "/capture/pinGroups";

constexpr std::array<const char*, PinGroupConstants::kPaletteColorCount> kPresetHexes = {
    "#3B82F6", // 1: 科技蓝
    "#10B981", // 2: 翡翠绿
    "#F59E0B", // 3: 琥珀金
    "#8B5CF6", // 4: 紫晶兰
    "#F43F5E", // 5: 珊瑚红
    "#06B6D4", // 6: 青碧蓝
    "#6366F1", // 7: 靛青蓝
    "#EC4899", // 8: 品红晶
    "#14B8A6", // 9: 松石青
    "#F97316", // 10: 曜阳橙
    "#84CC16", // 11: 青柠绿
    "#0EA5E9", // 12: 天际蓝
};

struct GroupCustomSlot {
    std::wstring name;
    std::string colorHex;
};

using SlotArray = std::array<GroupCustomSlot, PinGroupConstants::kMaxGroupId + 1>;

struct PaletteStore {
    std::mutex mutex; // 纯叶子锁：持锁期间严禁调用 ConfigManager 或任何外部回调
    bool loaded = false;
    std::atomic<bool> writingSelf{false};
    SlotArray slots{};
};

PaletteStore& store() {
    static PaletteStore s;
    return s;
}

int hexDigitValue(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return -1;
}

bool parseNormalizedHexRgb(std::string_view hex, float& rOut, float& gOut, float& bOut) noexcept {
    if (hex.size() != 7 || hex[0] != '#') return false;
    const int rHi = hexDigitValue(hex[1]);
    const int rLo = hexDigitValue(hex[2]);
    const int gHi = hexDigitValue(hex[3]);
    const int gLo = hexDigitValue(hex[4]);
    const int bHi = hexDigitValue(hex[5]);
    const int bLo = hexDigitValue(hex[6]);
    if (rHi < 0 || rLo < 0 || gHi < 0 || gLo < 0 || bHi < 0 || bLo < 0) return false;
    rOut = static_cast<float>((rHi << 4) | rLo) / 255.0f;
    gOut = static_cast<float>((gHi << 4) | gLo) / 255.0f;
    bOut = static_cast<float>((bHi << 4) | bLo) / 255.0f;
    return true;
}

SlotArray parseSlotsFromConfig() {
    SlotArray parsed{};
    const auto rawObj = tools3000::core::ConfigManager::instance().get<nlohmann::json>(
        kPinGroupsConfigKey, nlohmann::json::object());
    if (!rawObj.is_object()) {
        return parsed;
    }

    for (auto it = rawObj.begin(); it != rawObj.end(); ++it) {
        int gid = 0;
        try {
            gid = std::stoi(it.key());
        } catch (...) {
            continue;
        }
        const int norm = PinGroupPalette::normalizeGroupId(gid);
        if (norm == PinGroupConstants::kUngroupedId || !it.value().is_object()) continue;

        GroupCustomSlot parsedSlot;
        if (const auto nameIt = it.value().find("name"); nameIt != it.value().end() && nameIt->is_string()) {
            parsedSlot.name = PinGroupPalette::normalizeCustomName(
                tools3000::core::WinUtils::utf8ToWstring(nameIt->get<std::string>()));
        }
        if (const auto colorIt = it.value().find("color"); colorIt != it.value().end() && colorIt->is_string()) {
            if (auto normHex = PinGroupPalette::normalizeHexColor(colorIt->get<std::string>()); normHex.has_value()) {
                parsedSlot.colorHex = std::move(*normHex);
            }
        }
        parsed[static_cast<size_t>(norm)] = std::move(parsedSlot);
    }
    return parsed;
}

nlohmann::json serializeSlotsLocked(const PaletteStore& st) {
    nlohmann::json obj = nlohmann::json::object();
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const auto& slot = st.slots[static_cast<size_t>(g)];
        if (slot.name.empty() && slot.colorHex.empty()) continue;
        nlohmann::json entry = nlohmann::json::object();
        if (!slot.name.empty()) {
            entry["name"] = tools3000::core::WinUtils::wstringToUtf8(slot.name);
        }
        if (!slot.colorHex.empty()) {
            entry["color"] = slot.colorHex;
        }
        obj[std::to_string(g)] = std::move(entry);
    }
    return obj;
}

void ensureLoaded() {
    auto& st = store();
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        if (st.loaded) return;
    }
    SlotArray parsed = parseSlotsFromConfig();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (!st.loaded) {
        st.slots = std::move(parsed);
        st.loaded = true;
    }
}

void persistSnapshot(const nlohmann::json& serialized) {
    auto& st = store();
    struct SelfWriteGuard {
        std::atomic<bool>& flag;
        explicit SelfWriteGuard(std::atomic<bool>& f) noexcept : flag(f) {
            flag.store(true, std::memory_order_release);
        }
        ~SelfWriteGuard() {
            flag.store(false, std::memory_order_release);
        }
    } guard(st.writingSelf);

    tools3000::core::ConfigManager::instance().set(kPinGroupsConfigKey, serialized);
}

} // namespace

std::optional<std::string> PinGroupPalette::normalizeHexColor(std::string_view raw) {
    size_t start = 0;
    size_t end = raw.size();
    while (start < end && std::isspace(static_cast<unsigned char>(raw[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
    std::string_view trimmed = raw.substr(start, end - start);
    if (trimmed.empty()) {
        return std::string{};
    }
    if (trimmed.front() == '#') {
        trimmed.remove_prefix(1);
    }
    if (trimmed.size() == 3) {
        std::string expanded = "#";
        expanded.reserve(7);
        for (char ch : trimmed) {
            if (hexDigitValue(ch) < 0) return std::nullopt;
            const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            expanded.push_back(upper);
            expanded.push_back(upper);
        }
        return expanded;
    }
    if (trimmed.size() == 6) {
        std::string normalized = "#";
        normalized.reserve(7);
        for (char ch : trimmed) {
            if (hexDigitValue(ch) < 0) return std::nullopt;
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        return normalized;
    }
    return std::nullopt;
}

std::wstring PinGroupPalette::normalizeCustomName(std::wstring_view raw) {
    std::wstring cleaned;
    cleaned.reserve((std::min)(raw.size(), PinGroupConstants::kMaxCustomGroupNameChars));
    for (wchar_t ch : raw) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            cleaned.push_back(L' ');
        } else if (ch >= 0x20) {
            cleaned.push_back(ch);
        }
    }
    size_t start = 0;
    size_t end = cleaned.size();
    while (start < end && std::iswspace(cleaned[start])) ++start;
    while (end > start && std::iswspace(cleaned[end - 1])) --end;
    if (start >= end) return {};
    std::wstring result = cleaned.substr(start, end - start);
    if (result.size() > PinGroupConstants::kMaxCustomGroupNameChars) {
        result.resize(PinGroupConstants::kMaxCustomGroupNameChars);
    }
    return result;
}

const std::array<const char*, PinGroupConstants::kPaletteColorCount>& PinGroupPalette::presetPaletteHexes() noexcept {
    return kPresetHexes;
}

const wchar_t* PinGroupPalette::presetColorName(int slot, bool chinese) noexcept {
    switch (slot) {
        case 1:  return chinese ? L"科技蓝" : L"Blue";
        case 2:  return chinese ? L"翡翠绿" : L"Emerald";
        case 3:  return chinese ? L"琥珀金" : L"Amber";
        case 4:  return chinese ? L"紫晶兰" : L"Violet";
        case 5:  return chinese ? L"珊瑚红" : L"Rose";
        case 6:  return chinese ? L"青碧蓝" : L"Cyan";
        case 7:  return chinese ? L"靛青蓝" : L"Indigo";
        case 8:  return chinese ? L"品红晶" : L"Pink";
        case 9:  return chinese ? L"松石青" : L"Teal";
        case 10: return chinese ? L"曜阳橙" : L"Orange";
        case 11: return chinese ? L"青柠绿" : L"Lime";
        case 12: return chinese ? L"天际蓝" : L"Sky";
        default: return chinese ? L"默认" : L"Default";
    }
}

const char* PinGroupPalette::defaultHex(int groupId) noexcept {
    const int slot = paletteSlot(groupId);
    if (slot >= 1 && slot <= PinGroupConstants::kPaletteColorCount) {
        return kPresetHexes[static_cast<size_t>(slot - 1)];
    }
    return "#64748B";
}

D2D1_COLOR_F PinGroupPalette::defaultColor(int groupId, float alpha) noexcept {
    float r = 100.0f / 255.0f;
    float g = 116.0f / 255.0f;
    float b = 139.0f / 255.0f;
    parseNormalizedHexRgb(defaultHex(groupId), r, g, b);
    return D2D1::ColorF(r, g, b, alpha);
}

D2D1_COLOR_F PinGroupPalette::accentColor(int groupId, float alpha) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) {
        return defaultColor(norm, alpha);
    }
    const std::string hex = customColorHex(norm);
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (!hex.empty() && parseNormalizedHexRgb(hex, r, g, b)) {
        return D2D1::ColorF(r, g, b, alpha);
    }
    return defaultColor(norm, alpha);
}

std::string PinGroupPalette::accentHex(int groupId) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) {
        return defaultHex(norm);
    }
    std::string hex = customColorHex(norm);
    return hex.empty() ? std::string(defaultHex(norm)) : hex;
}

std::wstring PinGroupPalette::shortLabel(int groupId, bool chinese) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) {
        return chinese ? L"未分组" : L"Ungrouped";
    }
    std::wstring cname = customName(norm);
    if (!cname.empty()) {
        return cname;
    }
    return chinese ? std::format(L"组 {}", norm) : std::format(L"G{}", norm);
}

std::wstring PinGroupPalette::fullLabel(int groupId, bool chinese) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) {
        return chinese ? L"未分组" : L"Ungrouped";
    }
    const std::wstring cname = customName(norm);
    if (!cname.empty()) {
        return chinese
            ? std::format(L"第 {} 组 · {}", norm, cname)
            : std::format(L"Group {} · {}", norm, cname);
    }
    const std::string chex = customColorHex(norm);
    if (!chex.empty()) {
        return chinese
            ? std::format(L"第 {} 组 ({})", norm, tools3000::core::WinUtils::utf8ToWstring(chex))
            : std::format(L"Group {} ({})", norm, tools3000::core::WinUtils::utf8ToWstring(chex));
    }
    return chinese
        ? std::format(L"第 {} 组 ({})", norm, colorName(norm, true))
        : std::format(L"Group {} ({})", norm, colorName(norm, false));
}

const wchar_t* PinGroupPalette::colorName(int groupId, bool chinese) noexcept {
    return presetColorName(paletteSlot(groupId), chinese);
}

void PinGroupPalette::ensureLoadedFromConfig() {
    ensureLoaded();
}

void PinGroupPalette::reloadFromConfig() {
    auto& st = store();
    if (st.writingSelf.load(std::memory_order_acquire)) {
        return;
    }
    SlotArray parsed = parseSlotsFromConfig();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.slots = std::move(parsed);
    st.loaded = true;
}

std::wstring PinGroupPalette::customName(int groupId) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return {};
    ensureLoaded();
    auto& st = store();
    std::lock_guard<std::mutex> lk(st.mutex);
    return st.slots[static_cast<size_t>(norm)].name;
}

std::string PinGroupPalette::customColorHex(int groupId) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return {};
    ensureLoaded();
    auto& st = store();
    std::lock_guard<std::mutex> lk(st.mutex);
    return st.slots[static_cast<size_t>(norm)].colorHex;
}

bool PinGroupPalette::hasCustomization(int groupId) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return false;
    ensureLoaded();
    auto& st = store();
    std::lock_guard<std::mutex> lk(st.mutex);
    const auto& slot = st.slots[static_cast<size_t>(norm)];
    return !slot.name.empty() || !slot.colorHex.empty();
}

bool PinGroupPalette::setGroupName(int groupId, std::wstring_view name, bool persist) {
    return setGroupCustomization(groupId, name, std::nullopt, persist);
}

bool PinGroupPalette::setGroupColor(int groupId, std::string_view colorHex, bool persist) {
    return setGroupCustomization(groupId, std::nullopt, colorHex, persist);
}

bool PinGroupPalette::setGroupCustomization(
    int groupId,
    std::optional<std::wstring_view> name,
    std::optional<std::string_view> colorHex,
    bool persist
) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return false;

    std::optional<std::string> validatedColor;
    if (colorHex.has_value()) {
        validatedColor = normalizeHexColor(*colorHex);
        if (!validatedColor.has_value()) {
            return false;
        }
    }

    ensureLoaded();

    nlohmann::json snapshotToPersist;
    {
        auto& st = store();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto& slot = st.slots[static_cast<size_t>(norm)];
        if (name.has_value()) {
            slot.name = normalizeCustomName(*name);
        }
        if (validatedColor.has_value()) {
            slot.colorHex = std::move(*validatedColor);
        }
        if (persist) {
            snapshotToPersist = serializeSlotsLocked(st);
        }
    }

    if (persist) {
        persistSnapshot(snapshotToPersist);
    }
    return true;
}

void PinGroupPalette::resetGroupCustomization(int groupId, bool persist) {
    const int norm = normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return;

    ensureLoaded();

    nlohmann::json snapshotToPersist;
    {
        auto& st = store();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto& slot = st.slots[static_cast<size_t>(norm)];
        slot.name.clear();
        slot.colorHex.clear();
        if (persist) {
            snapshotToPersist = serializeSlotsLocked(st);
        }
    }

    if (persist) {
        persistSnapshot(snapshotToPersist);
    }
}

void PinGroupPalette::resetAllCustomizations(bool persist) {
    ensureLoaded();

    nlohmann::json snapshotToPersist = nlohmann::json::object();
    {
        auto& st = store();
        std::lock_guard<std::mutex> lk(st.mutex);
        for (auto& slot : st.slots) {
            slot.name.clear();
            slot.colorHex.clear();
        }
    }
    if (persist) {
        persistSnapshot(snapshotToPersist);
    }
}

} // namespace tools3000::capture

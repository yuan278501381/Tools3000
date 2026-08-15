#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureProfile — 手势配置集
//
// 每个 Profile 包含一组手势 → 动作的映射。
// 全局默认 Profile + 按应用的专属 Profile（如浏览器专用）。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTUREPROFILE_H
#define TOOLS3000_GESTURE_GESTUREPROFILE_H

#include "gesture/GestureAction.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>

namespace tools3000::gesture {

/// 触发方式三态模型
enum class TriggerModeState : uint8_t {
    Default = 0,   // 继承全局默认
    Enabled = 1,   // 强制启用
    Disabled = 2   // 强制禁用
};

inline std::string triggerStateToString(TriggerModeState state) {
    switch (state) {
        case TriggerModeState::Enabled:  return "enabled";
        case TriggerModeState::Disabled: return "disabled";
        case TriggerModeState::Default:
        default:                         return "default";
    }
}

inline TriggerModeState triggerStateFromString(const std::string& str) {
    if (str == "enabled" || str == "1")  return TriggerModeState::Enabled;
    if (str == "disabled" || str == "2") return TriggerModeState::Disabled;
    return TriggerModeState::Default;
}

class GestureProfile {
public:
    explicit GestureProfile(const std::string& name = "default");

    /// Profile 名称
    const std::string& name() const { return m_name; }

    /// 添加或更新手势映射
    void addMapping(const GestureMapping& mapping);

    /// 删除手势映射
    void removeMapping(const std::string& gestureCode);

    /// 启用/禁用单个手势
    void setMappingEnabled(const std::string& gestureCode, bool enabled);

    /// 根据手势编码查找生效的动作 (若手势被禁用则返回 nullopt)
    std::optional<GestureAction> findAction(const std::string& gestureCode) const;

    /// 查找完整手势映射条目
    std::optional<GestureMapping> findMapping(const std::string& gestureCode) const;

    /// 调整手势顺序 (通过新的手势编码顺序列表)
    void reorderMappings(const std::vector<std::string>& orderedCodes);

    /// 移动手势顺序 (上移/下移)
    bool moveMapping(size_t fromIndex, size_t toIndex);

    /// 检查手势编码是否已存在
    bool hasGesture(const std::string& gestureCode) const;

    /// 检测手势冲突（前缀冲突: "L" 和 "L-U" 冲突）
    std::vector<std::string> detectConflicts(const std::string& gestureCode) const;

    /// 获取所有映射
    const std::vector<GestureMapping>& getMappings() const { return m_mappings; }

    /// 清除所有映射
    void clearMappings() { m_mappings.clear(); m_codeIndex.clear(); }

    /// 触发方式三态管控
    TriggerModeState getTriggerState(const std::string& triggerKey) const;
    void setTriggerState(const std::string& triggerKey, TriggerModeState state);
    void setAllTriggerStates(TriggerModeState state);
    const std::unordered_map<std::string, TriggerModeState>& getAllTriggerStates() const { return m_triggerStates; }

    /// 创建默认全局 Profile
    static GestureProfile createDefaultGlobal();

    /// 创建浏览器专用 Profile
    static GestureProfile createBrowserProfile();

    /// 创建桌面特殊目标专属 Profile
    static GestureProfile createDesktopProfile();

    /// 创建任务栏特殊目标专属 Profile
    static GestureProfile createTaskbarProfile();

    /// 序列化
    nlohmann::json toJson() const;
    static GestureProfile fromJson(const nlohmann::json& j);

private:
    std::string m_name;
    std::vector<GestureMapping> m_mappings;
    std::unordered_map<std::string, size_t> m_codeIndex;  // gestureCode → m_mappings 索引
    std::unordered_map<std::string, TriggerModeState> m_triggerStates; // 触发方式三态状态表

    void rebuildIndex();
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_GESTUREPROFILE_H

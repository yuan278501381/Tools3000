#pragma once
#ifndef TOOLS3000_CORE_HOTKEY_MODIFIERDOUBLETAPDETECTOR_H
#define TOOLS3000_CORE_HOTKEY_MODIFIERDOUBLETAPDETECTOR_H

// ─────────────────────────────────────────────────────────────────────────────
// ModifierDoubleTapDetector.h — 零 Win32 句柄依赖的通用修饰键双击状态机 (SSOT)
//
// 职责:
//   1. 为 Ctrl / Alt / Shift 三类纯修饰键提供严格 Down -> Up -> Down 双击闭环检测；
//   2. 五重防误触护栏：过滤硬件 Auto-Repeat、首按长按熔断 (<=280ms)、间隔窗口校验
//      (25~380ms)、多修饰键/杂键污染熔断、鼠标点击/滚轮活动熔断与 AltGr 假 LCtrl 隔离；
//   3. 固定 <128 字节内联栈状态、零堆分配、零锁热路径，支持 100% 离线确定性单元测试。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <cstdint>

namespace tools3000::core {

enum class DoubleTapModifier : uint8_t {
    None  = 0,
    Ctrl  = 1,
    Alt   = 2,
    Shift = 3
};

namespace DoubleTapConstants {
constexpr uint8_t kCtrlMask  = 1u << 0;
constexpr uint8_t kAltMask   = 1u << 1;
constexpr uint8_t kShiftMask = 1u << 2;
constexpr uint8_t kAllMask   = kCtrlMask | kAltMask | kShiftMask;

constexpr int64_t kLongHoldAbortMs    = 260; // Auto-Repeat 持续按压熔断阈值
constexpr int64_t kMaxFirstTapHoldMs  = 280; // 首次轻触最长按住时长
constexpr int64_t kMinUpToDownGapMs   = 25;  // 防机械微动抖动最小间隔
constexpr int64_t kMaxUpToDownGapMs   = 380; // 两次敲击最大允许间隔

// 虚拟键码常量（避免头文件强耦合 <windows.h>）
constexpr uint32_t kVkShift    = 0x10;
constexpr uint32_t kVkControl  = 0x11;
constexpr uint32_t kVkMenu     = 0x12;
constexpr uint32_t kVkLShift   = 0xA0;
constexpr uint32_t kVkRShift   = 0xA1;
constexpr uint32_t kVkLControl = 0xA2;
constexpr uint32_t kVkRControl = 0xA3;
constexpr uint32_t kVkLMenu    = 0xA4;
constexpr uint32_t kVkRMenu    = 0xA5;
} // namespace DoubleTapConstants

class ModifierDoubleTapDetector {
public:
    static constexpr DoubleTapModifier classifyModifierVk(uint32_t vk) noexcept {
        using namespace DoubleTapConstants;
        if (vk == kVkControl || vk == kVkLControl || vk == kVkRControl) {
            return DoubleTapModifier::Ctrl;
        }
        if (vk == kVkMenu || vk == kVkLMenu || vk == kVkRMenu) {
            return DoubleTapModifier::Alt;
        }
        if (vk == kVkShift || vk == kVkLShift || vk == kVkRShift) {
            return DoubleTapModifier::Shift;
        }
        return DoubleTapModifier::None;
    }

    static constexpr uint8_t maskFor(DoubleTapModifier mod) noexcept {
        switch (mod) {
            case DoubleTapModifier::Ctrl:  return DoubleTapConstants::kCtrlMask;
            case DoubleTapModifier::Alt:   return DoubleTapConstants::kAltMask;
            case DoubleTapModifier::Shift: return DoubleTapConstants::kShiftMask;
            default:                       return 0;
        }
    }

    /// 处理一次键盘事件；若命中某启用中的修饰键双击闭环，返回对应 DoubleTapModifier，否则返回 None。
    DoubleTapModifier onKeyEvent(
        uint32_t vk,
        bool isDown,
        bool isUp,
        int64_t nowMs,
        uint8_t activeMask = DoubleTapConstants::kAllMask,
        bool ignoreSyntheticAltGrCtrl = false) noexcept {
        if (ignoreSyntheticAltGrCtrl) {
            return DoubleTapModifier::None;
        }

        const DoubleTapModifier mod = classifyModifierVk(vk);
        const int idx = slotIndex(mod);

        if (isDown) {
            if (idx < 0) {
                // 任意非修饰主键（字母、数字、Tab、Space 等）立即熔断全部双击候选态
                abortAllTapStates();
                return DoubleTapModifier::None;
            }

            // 若有其他修饰键正处于物理按下态（如 Ctrl+Shift / Alt+Ctrl），属于多修饰键组合，立即熔断
            for (int i = 0; i < 3; ++i) {
                if (i != idx && m_physDown[i]) {
                    m_physDown[idx] = true;
                    abortAllTapStates();
                    return DoubleTapModifier::None;
                }
            }

            Slot& slot = m_slots[idx];

            // 1. 硬件 Auto-Repeat 静默过滤：已按下状态重复 KeyDown 绝不当作第二次点击
            if (m_physDown[idx]) {
                if (slot.state == TapState::FirstPressed &&
                    (nowMs - slot.firstDownMs) > DoubleTapConstants::kLongHoldAbortMs) {
                    slot = {};
                }
                return DoubleTapModifier::None;
            }

            m_physDown[idx] = true;

            // 清空其他两类修饰键的待机候选，防止 Ctrl -> Alt -> Ctrl 穿插误判
            for (int i = 0; i < 3; ++i) {
                if (i != idx) {
                    m_slots[i] = {};
                }
            }

            if ((activeMask & maskFor(mod)) == 0) {
                slot = {};
                return DoubleTapModifier::None;
            }

            if (slot.state == TapState::WaitingSecond) {
                const int64_t gap = nowMs - slot.firstUpMs;
                if (gap >= DoubleTapConstants::kMinUpToDownGapMs &&
                    gap <= DoubleTapConstants::kMaxUpToDownGapMs) {
                    slot = {};
                    return mod;
                }
            }

            slot.state = TapState::FirstPressed;
            slot.firstDownMs = nowMs;
            slot.firstUpMs = 0;
            return DoubleTapModifier::None;
        }

        if (isUp && idx >= 0) {
            m_physDown[idx] = false;
            Slot& slot = m_slots[idx];
            if (slot.state == TapState::FirstPressed) {
                const int64_t holdMs = nowMs - slot.firstDownMs;
                if (holdMs >= 0 && holdMs <= DoubleTapConstants::kMaxFirstTapHoldMs) {
                    slot.state = TapState::WaitingSecond;
                    slot.firstUpMs = nowMs;
                } else {
                    slot = {};
                }
            }
        }

        return DoubleTapModifier::None;
    }

    /// 鼠标点击或滚轮活动立即熔断双击候选态（防止 Ctrl+点击 / Ctrl+滚轮缩放误触发）
    void onMouseActivity() noexcept {
        abortAllTapStates();
    }

    /// 完全重置状态机与物理按键位图（用于暂停、全屏静默、远控让位、卸载恢复）
    void reset() noexcept {
        m_physDown = {false, false, false};
        abortAllTapStates();
    }

private:
    enum class TapState : uint8_t {
        Idle = 0,
        FirstPressed = 1,
        WaitingSecond = 2
    };

    struct Slot {
        TapState state = TapState::Idle;
        int64_t firstDownMs = 0;
        int64_t firstUpMs = 0;
    };

    static constexpr int slotIndex(DoubleTapModifier mod) noexcept {
        switch (mod) {
            case DoubleTapModifier::Ctrl:  return 0;
            case DoubleTapModifier::Alt:   return 1;
            case DoubleTapModifier::Shift: return 2;
            default:                       return -1;
        }
    }

    void abortAllTapStates() noexcept {
        for (auto& s : m_slots) {
            s = {};
        }
    }

    std::array<bool, 3> m_physDown{false, false, false};
    std::array<Slot, 3> m_slots{};
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_HOTKEY_MODIFIERDOUBLETAPDETECTOR_H

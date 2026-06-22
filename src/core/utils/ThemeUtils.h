#pragma once

#include <string>

namespace tools3000::core {

struct AccentColorRGB {
    float r;
    float g;
    float b;
};

/**
 * 获取强调色的归一化 RGB 浮点数值 (0.0f - 1.0f)
 */
inline AccentColorRGB getAccentColorRGB(const std::string& accent) noexcept {
    if (accent == "cyan")   return { 0.024f, 0.714f, 0.831f }; // #06b6d4 极光青
    if (accent == "amber")  return { 0.961f, 0.620f, 0.043f }; // #f59e0b 曜石金
    if (accent == "blue")   return { 0.231f, 0.510f, 0.965f }; // #3b82f6 深海蓝 (默认)
    if (accent == "mint")   return { 0.063f, 0.725f, 0.506f }; // #10b981 薄荷绿
    if (accent == "coral")  return { 0.957f, 0.247f, 0.369f }; // #f43f5e 暮霞珊瑚
    if (accent == "violet") return { 0.545f, 0.361f, 0.965f }; // #8b5cf6 经典魅紫
    return { 0.231f, 0.510f, 0.965f };                         // #3b82f6 深海蓝 (默认)
}

/**
 * 解析用户自定义 HEX 颜色字符串为归一化 RGB 浮点数值
 */
inline AccentColorRGB parseHexColor(const std::string& hex) noexcept {
    const size_t offset = !hex.empty() && hex.front() == '#' ? 1u : 0u;
    if (hex.size() == offset + 6u) {
        const auto hexNibble = [](const char ch) noexcept -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };

        unsigned long value = 0;
        for (size_t index = offset; index < hex.size(); ++index) {
            const int nibble = hexNibble(hex[index]);
            if (nibble < 0) return { 0.231f, 0.510f, 0.965f };
            value = (value << 4u) | static_cast<unsigned long>(nibble);
        }
        return {
            static_cast<float>((value >> 16) & 0xFF) / 255.0f,
            static_cast<float>((value >> 8) & 0xFF) / 255.0f,
            static_cast<float>(value & 0xFF) / 255.0f
        };
    }
    return { 0.231f, 0.510f, 0.965f };
}

}  // namespace tools3000::core

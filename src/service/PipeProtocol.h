#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PipeProtocol — 搜索服务命名管道分帧协议
//
// 字节流搭配 4 字节小端长度前缀：任意大小的响应都能分块完整传输，
// 且帧边界由长度显式给出，不再依赖内核缓冲区大小。
// 单次客户端 JSON 请求包上限收紧至 1 MB，阻断超大恶意帧内存耗尽攻击。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SERVICE_PIPEPROTOCOL_H
#define TOOLS3000_SERVICE_PIPEPROTOCOL_H

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace tools3000::service::pipe {

/// 长度前缀占用的字节数。
inline constexpr std::size_t HeaderSize = 4;

/// 单帧响应净荷上限。损坏或伪造的长度前缀不会导致巨量分配。
inline constexpr std::uint32_t MaxFrameBytes = 64u * 1024u * 1024u;

/// 单次客户端 JSON 请求帧上限（1 MB）。
inline constexpr std::uint32_t MaxRequestBytes = 1024u * 1024u;

/// Windows named pipes are byte streams; a single ReadFile/WriteFile is not a
/// frame operation. Keep the completion loops beside the framing contract so
/// every endpoint handles fragmented I/O identically.
inline constexpr DWORD IoChunkBytes = 64u * 1024u;

inline bool readExact(HANDLE pipe, char* buffer, std::size_t bytes) noexcept {
    if (pipe == INVALID_HANDLE_VALUE || !buffer || bytes == 0) return false;
    std::size_t total = 0;
    while (total < bytes) {
        const DWORD wanted = static_cast<DWORD>((std::min<std::size_t>)(bytes - total, IoChunkBytes));
        DWORD received = 0;
        if (!ReadFile(pipe, buffer + total, wanted, &received, nullptr) || received == 0) return false;
        total += received;
    }
    return true;
}

inline bool writeExact(HANDLE pipe, const char* data, std::size_t bytes) noexcept {
    if (pipe == INVALID_HANDLE_VALUE || !data || bytes == 0) return false;
    std::size_t total = 0;
    while (total < bytes) {
        const DWORD wanted = static_cast<DWORD>((std::min<std::size_t>)(bytes - total, IoChunkBytes));
        DWORD sent = 0;
        if (!WriteFile(pipe, data + total, wanted, &sent, nullptr) || sent == 0) return false;
        total += sent;
    }
    return true;
}

/// 将净荷长度编码为小端长度前缀。
inline std::array<char, HeaderSize> encodeFrameHeader(std::uint32_t payloadBytes) noexcept {
    return {
        static_cast<char>(payloadBytes & 0xFFu),
        static_cast<char>((payloadBytes >> 8) & 0xFFu),
        static_cast<char>((payloadBytes >> 16) & 0xFFu),
        static_cast<char>((payloadBytes >> 24) & 0xFFu),
    };
}

/// 解析长度前缀。长度为 0 或超过 limit 时判定为损坏帧并返回 false。
inline bool decodeFrameHeader(const char* header, std::uint32_t& payloadBytes,
                              std::uint32_t limit = MaxFrameBytes) noexcept {
    if (header == nullptr) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(header);
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    if (value == 0 || value > limit) return false;
    payloadBytes = value;
    return true;
}

/// 净荷是否可以装进一帧。用于发送前的前置校验。
inline bool fitsInFrame(std::size_t payloadBytes, std::uint32_t limit = MaxFrameBytes) noexcept {
    return payloadBytes > 0 && payloadBytes <= static_cast<std::size_t>(limit);
}

}  // namespace tools3000::service::pipe

#endif  // TOOLS3000_SERVICE_PIPEPROTOCOL_H

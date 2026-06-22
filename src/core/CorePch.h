#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CorePch.h — Tools3000 全局 C++ 预编译头 (Precompiled Header)
//
// 预编译核心头文件体系：
//   1. Windows SDK 核心头文件 (windows.h, shlwapi.h, shellapi.h, dwmapi.h)
//   2. C++20 高频 STL 容器与工具 (string, vector, memory, unordered_map, filesystem 等)
//   3. 高频第三方依赖库 (fmt, spdlog, nlohmann/json)
//
// 作用：
//   消除各编译单元对数万行重型系统头文件与模板库的重复解析，结合 /Z7 实现
//   满载多核并行编译加速与 sccache 编译器级缓存复用。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Windows SDK 核心
#include <windows.h>
#include <ole2.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>

// C++20 STL 高频组件
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>
#include <cstdint>
#include <sstream>

// 高频第三方库
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

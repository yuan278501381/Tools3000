#pragma once

#ifndef TOOLS3000_CORE_PLUGIN_PLUGINMANIFEST_H
#define TOOLS3000_CORE_PLUGIN_PLUGINMANIFEST_H

#include "core/utils/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tools3000::core {

inline constexpr std::uint32_t CurrentPluginManifestSchema = 2;
inline constexpr std::uint32_t CurrentPluginAbiVersion = 1;

struct PluginManifest {
    std::uint32_t schemaVersion = 0;
    std::uint32_t abiVersion = 0;
    std::string id;
    std::string name;
    std::string version;
    std::string minimumHostVersion;
    std::string entryPoint;
    std::string executionModel;
    std::vector<std::string> capabilities;
    std::vector<std::string> permissions;
};

struct PluginManifestResult {
    PluginManifest manifest;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

/// Parse and validate a sidecar manifest without mapping the plugin DLL.
TOOLS3000CORE_API PluginManifestResult loadPluginManifest(
    const std::filesystem::path& path,
    const std::string& expectedId,
    const std::string& hostVersion);

/// Strict dotted numeric version comparison. Returns -1, 0, or 1.
TOOLS3000CORE_API int comparePluginVersions(const std::string& left,
                                       const std::string& right) noexcept;

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_PLUGIN_PLUGINMANIFEST_H

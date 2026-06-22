#pragma once

#include "core/utils/Export.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace tools3000::core {

struct RenamePathResult {
    bool success = false;
    std::filesystem::path newPath;
    std::string error;
};

// Renames one existing filesystem entry without allowing the caller-provided
// name to select another directory, drive, device name, or NTFS stream.
TOOLS3000CORE_API RenamePathResult renamePathWithinParent(
    const std::filesystem::path& oldPath, std::wstring_view newName);

}  // namespace tools3000::core

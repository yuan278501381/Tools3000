#include "core/plugin/PluginManifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <unordered_set>

namespace tools3000::core {
namespace {

constexpr std::size_t MaxManifestBytes = 64 * 1024;
constexpr std::size_t MaxTextLength = 128;
constexpr std::size_t MaxListItems = 64;

std::optional<std::array<std::uint32_t, 3>> parseVersion(const std::string& value) noexcept {
    std::array<std::uint32_t, 3> result{};
    std::size_t part = 0;
    std::uint64_t number = 0;
    bool hasDigit = false;
    for (const unsigned char ch : value) {
        if (std::isdigit(ch)) {
            hasDigit = true;
            number = number * 10 + static_cast<unsigned>(ch - '0');
            if (number > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        } else if (ch == '.' && hasDigit && part < result.size() - 1) {
            result[part++] = static_cast<std::uint32_t>(number);
            number = 0;
            hasDigit = false;
        } else {
            return std::nullopt;
        }
    }
    if (!hasDigit) return std::nullopt;
    result[part] = static_cast<std::uint32_t>(number);
    return result;
}

bool validToken(const std::string& value) {
    if (value.empty() || value.size() > MaxTextLength) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
}

bool readString(const nlohmann::json& json, const char* key, std::string& value) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return false;
    value = it->get<std::string>();
    return !value.empty() && value.size() <= MaxTextLength;
}

bool readTokenList(const nlohmann::json& json, const char* key,
                   std::vector<std::string>& output) {
    const auto it = json.find(key);
    if (it == json.end()) return true;
    if (!it->is_array() || it->size() > MaxListItems) return false;
    std::unordered_set<std::string> unique;
    for (const auto& item : *it) {
        if (!item.is_string()) return false;
        auto value = item.get<std::string>();
        if (!validToken(value) || !unique.insert(value).second) return false;
        output.push_back(std::move(value));
    }
    return true;
}

PluginManifestResult fail(std::string message) {
    PluginManifestResult result;
    result.error = std::move(message);
    return result;
}

}  // namespace

int comparePluginVersions(const std::string& left, const std::string& right) noexcept {
    const auto lhs = parseVersion(left);
    const auto rhs = parseVersion(right);
    if (!lhs || !rhs) return 0;
    if (*lhs < *rhs) return -1;
    if (*lhs > *rhs) return 1;
    return 0;
}

PluginManifestResult loadPluginManifest(const std::filesystem::path& path,
                                        const std::string& expectedId,
                                        const std::string& hostVersion) {
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError) return fail("missing plugin manifest");
    if (size == 0 || size > MaxManifestBytes) return fail("invalid plugin manifest size");

    nlohmann::json json;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return fail("cannot open plugin manifest");
        stream >> json;
    } catch (const std::exception&) {
        return fail("malformed plugin manifest");
    }
    if (!json.is_object()) return fail("plugin manifest must be an object");

    PluginManifest manifest;
    const auto schema = json.find("schemaVersion");
    const auto abi = json.find("abiVersion");
    if (schema == json.end() || !schema->is_number_unsigned() ||
        abi == json.end() || !abi->is_number_unsigned()) {
        return fail("plugin manifest is missing version fields");
    }
    manifest.schemaVersion = schema->get<std::uint32_t>();
    manifest.abiVersion = abi->get<std::uint32_t>();
    if (manifest.schemaVersion != CurrentPluginManifestSchema) {
        return fail("unsupported plugin manifest schema");
    }
    if (manifest.abiVersion != CurrentPluginAbiVersion) {
        return fail("incompatible plugin ABI");
    }
    if (!readString(json, "id", manifest.id) || !validToken(manifest.id) ||
        manifest.id != expectedId) {
        return fail("plugin manifest id does not match its DLL");
    }
    if (!readString(json, "name", manifest.name) ||
        !readString(json, "version", manifest.version) ||
        !readString(json, "minimumHostVersion", manifest.minimumHostVersion) ||
        !readString(json, "entryPoint", manifest.entryPoint)) {
        return fail("plugin manifest is missing metadata");
    }
    if (!parseVersion(manifest.version) || !parseVersion(manifest.minimumHostVersion) ||
        !parseVersion(hostVersion)) {
        return fail("plugin manifest contains an invalid version");
    }
    if (comparePluginVersions(hostVersion, manifest.minimumHostVersion) < 0) {
        return fail("plugin requires a newer Tools3000 version");
    }
    if (manifest.entryPoint != "CreatePlugin") {
        return fail("unsupported plugin entry point");
    }
    if (!readString(json, "executionModel", manifest.executionModel) ||
        manifest.executionModel != "trusted-native-in-process") {
        return fail("plugin must declare the trusted native in-process execution model");
    }
    if (!readTokenList(json, "capabilities", manifest.capabilities) ||
        !readTokenList(json, "permissions", manifest.permissions)) {
        return fail("plugin manifest contains an invalid capability or permission");
    }

    return {std::move(manifest), {}};
}

}  // namespace tools3000::core

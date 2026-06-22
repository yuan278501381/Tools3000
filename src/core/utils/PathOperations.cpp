#include "core/utils/PathOperations.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <system_error>

namespace tools3000::core {
namespace {

bool isReservedWindowsName(std::wstring_view name) {
    const auto dot = name.find(L'.');
    std::wstring base(name.substr(0, dot));
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });

    static constexpr std::array<std::wstring_view, 4> fixedNames = {
        L"CON", L"PRN", L"AUX", L"NUL"
    };
    if (std::find(fixedNames.begin(), fixedNames.end(), base) != fixedNames.end()) {
        return true;
    }
    if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))) {
        const wchar_t suffix = base[3];
        if ((suffix >= L'1' && suffix <= L'9') || suffix == L'\u00B9' ||
            suffix == L'\u00B2' || suffix == L'\u00B3') {
            return true;
        }
    }
    return false;
}

std::string validateLeafName(std::wstring_view newName) {
    if (newName.empty()) return "new name is required";
    if (newName.size() > 255) return "new name exceeds the Windows component limit";
    if (newName == L"." || newName == L"..") return "new name must not be '.' or '..'";
    if (newName.back() == L'.' || newName.back() == L' ') {
        return "new name must not end with a dot or space";
    }
    for (const wchar_t ch : newName) {
        if (ch < 32 || ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            return "new name contains invalid Windows filename characters";
        }
    }
    if (isReservedWindowsName(newName)) return "new name is reserved by Windows";

    const std::filesystem::path leaf(newName);
    if (leaf.is_absolute() || leaf.has_root_name() || leaf.has_root_directory() ||
        leaf.has_parent_path() || leaf.filename().native() != newName) {
        return "new name must be a single filename";
    }
    return {};
}

}  // namespace

RenamePathResult renamePathWithinParent(const std::filesystem::path& oldPath,
                                        std::wstring_view newName) {
    RenamePathResult result;
    if (const std::string validationError = validateLeafName(newName);
        !validationError.empty()) {
        result.error = validationError;
        return result;
    }

    std::error_code error;
    if (!std::filesystem::exists(oldPath, error)) {
        result.error = error ? error.message() : "source file or directory does not exist";
        return result;
    }

    auto parent = oldPath.parent_path();
    if (parent.empty()) {
        parent = std::filesystem::absolute(oldPath, error).parent_path();
        if (error || parent.empty()) {
            result.error = error ? error.message() : "source path has no parent directory";
            return result;
        }
    }

    result.newPath = parent / std::filesystem::path(newName);
    // MoveFileW has atomic no-replace semantics for the destination.  A separate
    // exists() check followed by std::filesystem::rename() is unsafe on MSVC:
    // another process can create the destination between the two calls and the
    // CRT implementation may then replace it.
    if (!MoveFileW(oldPath.c_str(), result.newPath.c_str())) {
        const DWORD lastError = GetLastError();
        if (lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS) {
            result.error = "destination already exists";
        } else {
            result.error = std::error_code(
                static_cast<int>(lastError), std::system_category()).message();
        }
        return result;
    }
    result.success = true;
    return result;
}

}  // namespace tools3000::core

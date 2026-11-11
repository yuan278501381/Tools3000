#include "core/ipc/FileInteractionHandlers.h"

#include "core/ipc/MessageBridge.h"
#include "core/utils/PathOperations.h"
#include "core/utils/ShellContextMenuService.h"
#include "core/utils/WinUtils.h"

#include <filesystem>
#include <string>

namespace tools3000::core {
namespace {

std::string requestPath(const json& params, bool preferFilepath) {
    return preferFilepath
        ? params.value("filepath", params.value("path", ""))
        : params.value("path", params.value("filepath", ""));
}

json renameRequest(const json& params) {
    const std::string oldPath = params.value("oldPath", params.value("path", ""));
    const std::string newName = params.value("newName", params.value("name", ""));
    if (oldPath.empty() || newName.empty()) {
        return {{"success", false}, {"error", "invalid parameters"}};
    }

    const auto result = renamePathWithinParent(
        WinUtils::utf8ToWstring(oldPath), WinUtils::utf8ToWstring(newName));
    if (!result.success) return {{"success", false}, {"error", result.error}};
    return {
        {"success", true},
        {"newPath", WinUtils::wstringToUtf8(result.newPath.wstring())},
        {"newName", newName}
    };
}

}  // namespace

void registerFileInteractionHandlers(MessageBridge& bridge,
                                     std::string_view methodNamespace,
                                     std::string_view missingPathError,
                                     bool preferFilepathParameter) {
    const std::string prefix(methodNamespace);
    const std::string pathError(missingPathError);
    const auto method = [&prefix](std::string_view action) {
        return prefix + "." + std::string(action);
    };
    const auto missingPath = [pathError]() -> json {
        return {{"success", false}, {"error", pathError}};
    };

    auto openFileHandler = [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        return {{"success", WinUtils::openFile(WinUtils::utf8ToWstring(path))}};
    };
    auto openFolderHandler = [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        return {{"success", WinUtils::openFolderAndSelectItem(WinUtils::utf8ToWstring(path))}};
    };
    auto openAsAdminHandler = [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        return {{"success", WinUtils::openFileAsAdmin(WinUtils::utf8ToWstring(path))}};
    };
    auto showPropertiesHandler = [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        return {{"success", WinUtils::showFileProperties(WinUtils::utf8ToWstring(path))}};
    };
    auto openWithNotepadHandler = [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        return {{"success", WinUtils::openWithNotepad(WinUtils::utf8ToWstring(path))}};
    };
    auto copyFileHandler = [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        const std::wstring wpath = WinUtils::utf8ToWstring(path);
        std::error_code ec;
        const bool isGif = !std::filesystem::is_directory(wpath, ec) &&
                           WinUtils::toLower(std::filesystem::path(wpath).extension().wstring()) == L".gif";
        const bool ok = isGif
            ? (WinUtils::copyGifToClipboard(wpath) || WinUtils::copyFileToClipboard(wpath))
            : WinUtils::copyFileToClipboard(wpath);
        return {{"success", ok}};
    };

    bridge.registerAsyncHandler(method("openFile"), openFileHandler);
    bridge.registerAsyncHandler(method("openFolder"), openFolderHandler);
    bridge.registerAsyncHandler(method("openFileAsAdmin"), openAsAdminHandler);
    bridge.registerAsyncHandler(method("showFileProperties"), showPropertiesHandler);
    bridge.registerAsyncHandler(method("openWithNotepad"), openWithNotepadHandler);
    bridge.registerAsyncHandler(method("copyFile"), copyFileHandler);
    if (methodNamespace == "search" || methodNamespace == "system") {
        bridge.registerAsyncHandler(method("openAsAdmin"), openAsAdminHandler);
        bridge.registerAsyncHandler(method("showProperties"), showPropertiesHandler);
    }
    bridge.registerHandler(method("renamePath"), renameRequest);
    bridge.registerHandler(method("showShellContextMenu"), [preferFilepathParameter, missingPath](const json& params) -> json {
        const auto path = requestPath(params, preferFilepathParameter);
        if (path.empty()) return missingPath();
        const bool started = ShellContextMenuService::instance().showAsync(
            WinUtils::utf8ToWstring(path), params.value("x", -1), params.value("y", -1),
            params.value("extended", false));
        return {{"success", started}, {"busy", !started}};
    });
}

}  // namespace tools3000::core

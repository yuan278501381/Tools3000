#include "core/ipc/SystemInteractionHandlers.h"

#include "core/ipc/FileInteractionHandlers.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/WinUtils.h"

#include <string>

namespace tools3000::core {

void registerSystemInteractionHandlers(MessageBridge& bridge) {
    registerFileInteractionHandlers(bridge, "system", "path is required");
    bridge.registerHandler("system.copyText", [](const json& params) -> json {
        const std::string text = params.value("text", "");
        return {{"success", WinUtils::copyToClipboard(text)}};
    });
    bridge.registerHandler("system.openUrl", [](const json& params) -> json {
        const std::string url = params.value("url", params.value("path", ""));
        if (url.empty()) return {{"success", false}, {"error", "url is required"}};
        return {{"success", WinUtils::openUrl(WinUtils::utf8ToWstring(url))}};
    });
    bridge.registerHandler("system.openExternal", [](const json& params) -> json {
        const std::string url = params.value("url", params.value("path", ""));
        if (url.empty()) return {{"success", false}, {"error", "url is required"}};
        return {{"success", WinUtils::openUrl(WinUtils::utf8ToWstring(url))}};
    });
}

}  // namespace tools3000::core

#pragma once

namespace tools3000::core {
class MessageBridge;
}

namespace tools3000::search {

void registerSearchInteractionHandlers(tools3000::core::MessageBridge& bridge);

}  // namespace tools3000::search

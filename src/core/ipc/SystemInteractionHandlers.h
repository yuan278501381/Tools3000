#pragma once

#include "core/utils/Export.h"

namespace tools3000::core {

class MessageBridge;

TOOLS3000CORE_API void registerSystemInteractionHandlers(MessageBridge& bridge);

}  // namespace tools3000::core

#pragma once

#include <AYApplication/GameFlowActionRegistry.h>

#include <string>
#include <string_view>

namespace ayt::app
{

// Parses the project-owned authoring contract and registers metadata only.
// Runtime handlers remain owned by the product composition root.
[[nodiscard]] bool loadGameFlowContract(
    std::string_view jsonText,
    GameFlowActionRegistry& registry,
    std::string* error = nullptr);

} // namespace ayt::app

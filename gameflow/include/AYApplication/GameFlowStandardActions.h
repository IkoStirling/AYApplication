#pragma once

#include <AYApplication/GameFlowActionRegistry.h>

#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

inline constexpr std::string_view kGameFlowActionWorldReplace =
    "world.replace";
inline constexpr std::string_view kGameFlowActionUIFlowStart =
    "ui.flow.start";
inline constexpr std::string_view kGameFlowActionUIContextActivate =
    "ui.context.activate";
inline constexpr std::string_view kGameFlowActionUIContextDeactivate =
    "ui.context.deactivate";
inline constexpr std::string_view kGameFlowActionUISignalEmit =
    "ui.signal.emit";

// Metadata-only registration is part of the headless GameFlow contract. Live
// World/UI adapters bind handlers in their optional targets.
[[nodiscard]] GameFlowActionTypeDefinition gameFlowWorldActionType();
[[nodiscard]] std::vector<GameFlowActionTypeDefinition>
gameFlowUIActionTypes();
[[nodiscard]] bool gameFlowActionTypeCompatible(
    const GameFlowActionTypeDefinition& value,
    const GameFlowActionTypeDefinition& expected) noexcept;
// Validates value-domain rules that cannot be expressed by scalar field
// types alone. Unknown project-owned actions are intentionally accepted.
[[nodiscard]] bool validateGameFlowStandardActionSemantics(
    const GameFlowActionCall& action,
    std::string* error = nullptr);
[[nodiscard]] bool registerGameFlowWorldActionType(
    GameFlowActionRegistry& registry,
    std::string* error = nullptr);
[[nodiscard]] bool registerGameFlowUIActionTypes(
    GameFlowActionRegistry& registry,
    std::string* error = nullptr);

} // namespace ayt::app

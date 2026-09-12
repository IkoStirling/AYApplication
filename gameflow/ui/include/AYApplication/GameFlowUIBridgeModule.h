#pragma once

#include <AYApplication/GameFlowUIBridge.h>
#include <AYGameLoop/SubSystemModule.h>

#include <string_view>

namespace ayt::app
{

class GameProject;
class IEngineHost;

inline constexpr std::string_view kGameFlowUIBridgeModuleId =
    "AYApplication.GameFlowUIBridge";
inline constexpr std::string_view kGameFlowUIBridgeSubSystemName =
    "GameFlowUIBridge";

class GameFlowUIBridgeModule final : public ayt::game::SubSystemModule
{
public:
    GameFlowUIBridgeModule(
        IEngineHost& host,
        GameFlowUIBridgeConfig config = {});

    void shutdown(ayt::module::IModuleContext& context) noexcept override;

private:
    IEngineHost& _host;
};

// Adds the UI action metadata before GameFlow preflight and installs the
// optional bridge after the project's UIFlowRuntime module. Existing project
// callbacks are preserved and run first.
void enableGameFlowUIBridge(
    GameProject& project,
    GameFlowUIBridgeConfig config = {});

[[nodiscard]] GameFlowUIBridge* gameFlowUIBridge(
    IEngineHost& host) noexcept;

} // namespace ayt::app

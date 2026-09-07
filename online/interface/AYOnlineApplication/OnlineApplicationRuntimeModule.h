#pragma once

#include <AYGameLoop/SubSystemModule.h>
#include <AYOnlineApplication/OnlineApplication.h>

#include <string_view>

namespace ayt::app
{
class EngineModuleRuntime;
}

namespace ayt::app::online
{

inline constexpr std::string_view kOnlineApplicationRuntimeModuleId =
    "AYOnlineApplication.Runtime";

// Optional client bridge. OnlineFlow and RuntimeSceneLoader are module
// dependencies; GameLoop still owns their initialize/update/shutdown order.
class OnlineApplicationRuntimeModule final
    : public ayt::game::SubSystemModule
{
public:
    OnlineApplicationRuntimeModule(
        ayt::app::IEngineHost& host,
        OnlineSceneBridgeConfig config);
};

// Adds Network -> Online -> OnlineFlow -> OnlineApplication to a Client graph.
// configureDefaultClientModules() must run first so RuntimeSceneLoader exists.
// Existing nodes with the same stable IDs are adopted instead of duplicated.
[[nodiscard]] ayt::module::ModuleResult configureOnlineApplicationModules(
    ayt::app::EngineModuleRuntime& runtime,
    OnlineApplicationConfig config,
    ayt::net::OnlineSubSystemDependencies dependencies = {});

} // namespace ayt::app::online

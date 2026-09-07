#pragma once

#include <AYApplication/RuntimeSceneLoader.h>
#include <AYGameLoop/SubSystemModule.h>

#include <string_view>

namespace ayt::event
{
class EventBus;
}

namespace ayt::scene
{
class SceneManager;
}

namespace ayt::app
{

inline constexpr std::string_view kRuntimeSceneLoaderModuleId =
    "AYApplication.RuntimeSceneLoader";

// Client-only module adapter. SceneManager remains a Host-owned service;
// GameLoop owns the created RuntimeSceneLoader subsystem after installation.
class RuntimeSceneLoaderModule final : public ayt::game::SubSystemModule
{
public:
    RuntimeSceneLoaderModule(
        ayt::scene::SceneManager& scenes,
        RuntimeSceneLoaderConfig config = {},
        ayt::event::EventBus* eventBus = nullptr);
};

} // namespace ayt::app

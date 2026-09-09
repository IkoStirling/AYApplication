#pragma once

#include <AYApplication/IApplication.h>
#include <AYModule/ModuleTypes.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::scene
{
class Scene;
}

namespace ayt::app
{

class EngineModuleRuntime;
class IEngineHost;

inline constexpr std::string_view kGameWorldRouterModuleId =
    "AYApplication.GameWorldRouter";

using PrepareWorldActivation = std::function<bool(
    ayt::scene::Scene& scene,
    std::string& error)>;

/// One stable game-facing World id mapped to one serialized Scene/World.
/// scenePath is relative to GameProject::assetRoot unless it is absolute.
struct GameWorld
{
    std::string id;
    std::string scenePath;
    std::string sceneName;
    PrepareWorldActivation prepareActivation;
};

/// The only engine assembly description a standalone game needs.
///
/// Build-time capabilities live in ay_add_engine_for_game(...). This object
/// chooses runtime behavior, the startup World, and project-owned modules.
struct GameProject
{
    std::string id = "game";
    std::string displayName = "Untitled";
    std::string version = "1.0.0";
    std::string assetRoot = "./assets";
    std::string userDataPath;

    uint32_t width = 1280;
    uint32_t height = 720;
    float targetFPS = 60.0f;
    bool enableRenderThread = true;
    bool enablePresentation = true;
    bool enablePhysics = true;
    bool serverMode = false;

    std::string startupWorld;
    std::vector<GameWorld> worlds;

    /// Add game-owned modules to the graph. The default engine graph and the
    /// GameWorldRouter node already exist when this callback runs.
    std::function<ayt::module::ModuleResult(EngineModuleRuntime&)>
        configureModules;
};

/// Narrow cross-World transition service. Game state that must survive a
/// transition belongs in a game module/service, not in the transient World.
class IGameWorldRouter
{
public:
    virtual ~IGameWorldRouter() = default;

    virtual bool requestWorld(std::string_view worldId) = 0;
    [[nodiscard]] virtual std::string_view currentWorldId() const noexcept = 0;
    [[nodiscard]] virtual std::string_view pendingWorldId() const noexcept = 0;
    [[nodiscard]] virtual std::string_view lastError() const noexcept = 0;
    [[nodiscard]] virtual const GameWorld* findWorld(
        std::string_view worldId) const noexcept = 0;
};

/// Validate stable ids, paths and the startup World without touching global
/// engine state. Empty error means success.
[[nodiscard]] bool validateGameProject(
    const GameProject& project,
    std::string& error);

/// Resolve the router published by the running game application.
[[nodiscard]] IGameWorldRouter* gameWorldRouter(IEngineHost& host) noexcept;

/// Run a standalone game through the standard Application assembly.
/// GameProject is taken by value so every string/callback remains valid for
/// the complete synchronous run.
int runGameProject(GameProject project, AppCommandLine commandLine = {});
int runGameProject(GameProject project, int argc, char* argv[]);
int runGameProject(GameProject project, int argc, wchar_t* argv[]);

} // namespace ayt::app

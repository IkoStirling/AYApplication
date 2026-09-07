#include <AYApplication/GameProject.h>

#include <AYApplication/EngineModuleContext.h>
#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RuntimeSceneLoader.h>
#include <AYApplication/RuntimeSceneLoaderModule.h>
#include <AYGameLoop/SubSystemModule.h>
#include <AYScene.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <utility>

namespace ayt::app
{
namespace
{

constexpr std::string_view kGameWorldRouterSubSystemName = "GameWorldRouter";

bool isTerminal(RuntimeSceneLoadState state) noexcept
{
    return state == RuntimeSceneLoadState::Ready ||
           state == RuntimeSceneLoadState::Failed ||
           state == RuntimeSceneLoadState::Cancelled;
}

class GameWorldRouterSubSystem final
    : public IGameWorldRouter,
      public ayt::game::ISubSystem
{
public:
    GameWorldRouterSubSystem(
        std::vector<GameWorld> worlds,
        std::string startupWorld)
        : _worlds(std::move(worlds)),
          _startupWorld(std::move(startupWorld))
    {
    }

    const char* getName() const override
    {
        return kGameWorldRouterSubSystemName.data();
    }

    const ayt::game::SubSystemDescriptor& getDescriptor() const override
    {
        static const ayt::game::SubSystemDescriptor descriptor{
            .name = "GameWorldRouter",
            .dependencies = {"RuntimeSceneLoader"},
            .basePriority = 1100,
            .timeType = ayt::game::SubSystemDescriptor::TimeType::Real,
            .phases = ayt::game::phaseBit(ayt::game::FramePhase::Egress),
            .clock = ayt::game::ClockDomain::RealWall,
            .initializeAfter = {"RuntimeSceneLoader"},
            .runsAfter = {"RuntimeSceneLoader"},
            .phasePriority = 1100,
            .reads = {"Application.SceneLoadStatus"},
            .writes = {"Game.CurrentWorldId"},
        };
        return descriptor;
    }

    bool initialize() override
    {
        _loader = findRegisteredRuntimeSceneLoader();
        if (_loader == nullptr) {
            _lastError = "RuntimeSceneLoader is unavailable";
            return false;
        }

        const RuntimeSceneLoadStatus startupStatus = _loader->getLoadStatus();
        if (startupStatus.state == RuntimeSceneLoadState::Failed) {
            _lastError = startupStatus.message.empty()
                ? "Startup World load failed"
                : startupStatus.message;
            return false;
        }

        const GameWorld* startup = findWorld(_startupWorld);
        if (startup == nullptr) {
            _lastError = "Startup World is not present in the catalog";
            return false;
        }

        if (startup->prepareActivation != nullptr) {
            ayt::scene::Scene* scene = _loader->currentScene();
            if (scene == nullptr ||
                !startup->prepareActivation(*scene, _lastError)) {
                if (_lastError.empty()) {
                    _lastError = "Startup World activation failed";
                }
                return false;
            }
        }

        _currentWorld = _startupWorld;
        _lastError.clear();
        return true;
    }

    void update(float) override
    {
        if (_loader == nullptr || _pendingWorld.empty()) {
            return;
        }

        const RuntimeSceneLoadStatus status = _loader->getLoadStatus();
        if (status.requestId != _pendingRequestId || !isTerminal(status.state)) {
            return;
        }

        if (status.state == RuntimeSceneLoadState::Ready) {
            _currentWorld = std::move(_pendingWorld);
            _pendingWorld.clear();
            _lastError.clear();
        } else {
            _lastError = status.message.empty()
                ? "World transition failed"
                : status.message;
            _pendingWorld.clear();
        }
        _pendingRequestId = 0;
    }

    void fixedUpdate(float) override {}

    void shutdown() override
    {
        if (_loader != nullptr && _pendingRequestId != 0) {
            _loader->cancelLoad(_pendingRequestId);
        }
        _loader = nullptr;
        _pendingRequestId = 0;
        _pendingWorld.clear();
        _currentWorld.clear();
        _lastError.clear();
    }

    bool requestWorld(std::string_view worldId) override
    {
        if (_loader == nullptr) {
            _lastError = "GameWorldRouter is not initialized";
            return false;
        }
        if (!_pendingWorld.empty()) {
            _lastError = "A World transition is already pending";
            return false;
        }
        if (worldId == _currentWorld) {
            _lastError.clear();
            return true;
        }

        const GameWorld* world = findWorld(worldId);
        if (world == nullptr) {
            _lastError = "Unknown World id: " + std::string(worldId);
            return false;
        }

        RuntimeSceneLoadRequest request;
        request.requestId = _nextRequestId++;
        request.scenePath = world->scenePath;
        request.sceneName = world->sceneName.empty()
            ? world->id
            : world->sceneName;
        request.prepareActivation = world->prepareActivation;
        if (!_loader->requestLoad(std::move(request))) {
            _lastError = "RuntimeSceneLoader rejected the World transition";
            return false;
        }

        _pendingRequestId = _nextRequestId - 1;
        _pendingWorld = world->id;
        _lastError.clear();
        return true;
    }

    std::string_view currentWorldId() const noexcept override
    {
        return _currentWorld;
    }

    std::string_view pendingWorldId() const noexcept override
    {
        return _pendingWorld;
    }

    std::string_view lastError() const noexcept override
    {
        return _lastError;
    }

    const GameWorld* findWorld(std::string_view worldId) const noexcept override
    {
        for (const GameWorld& world : _worlds) {
            if (world.id == worldId) {
                return &world;
            }
        }
        return nullptr;
    }

private:
    std::vector<GameWorld> _worlds;
    std::string _startupWorld;
    std::string _currentWorld;
    std::string _pendingWorld;
    std::string _lastError;
    IRuntimeSceneLoader* _loader = nullptr;
    uint64_t _nextRequestId = 1;
    uint64_t _pendingRequestId = 0;
};

class GameWorldRouterModule final : public ayt::game::SubSystemModule
{
public:
    GameWorldRouterModule(
        IEngineHost& host,
        std::vector<GameWorld> worlds,
        std::string startupWorld)
        : SubSystemModule(
              ayt::module::ModuleDescriptor{
                  .id = std::string(kGameWorldRouterModuleId),
                  .displayName = "Game World Router",
                  .version = "1.0.0",
                  .dependencies = {
                      ayt::module::ModuleDependency::required(
                          std::string(kRuntimeSceneLoaderModuleId))}},
              std::string(kGameWorldRouterSubSystemName),
              [worlds = std::move(worlds),
               startupWorld = std::move(startupWorld)]() mutable {
                  return std::make_unique<GameWorldRouterSubSystem>(
                      std::move(worlds), std::move(startupWorld));
              },
              [&host](
                  ayt::module::IModuleContext&,
                  ayt::game::ISubSystem& system) {
                  auto* router = dynamic_cast<IGameWorldRouter*>(&system);
                  if (router == nullptr) {
                      return ayt::module::ModuleResult::failure(
                          ayt::module::ModuleErrorCode::InstallationFailed,
                          "GameWorldRouter subsystem has an invalid interface");
                  }
                  host.provideService(kHostServiceGameWorldRouter, router);
                  return ayt::module::ModuleResult::success();
              }),
          _host(host)
    {
    }

    void shutdown(ayt::module::IModuleContext& context) noexcept override
    {
        try {
            if (_host.findService(kHostServiceGameWorldRouter) ==
                dynamic_cast<IGameWorldRouter*>(installedSubSystem())) {
                _host.provideService(kHostServiceGameWorldRouter, nullptr);
            }
        } catch (...) {
        }
        SubSystemModule::shutdown(context);
    }

private:
    IEngineHost& _host;
};

std::vector<GameWorld> resolveWorldPaths(
    const std::vector<GameWorld>& worlds,
    const std::string& assetRoot)
{
    std::vector<GameWorld> resolved = worlds;
    for (GameWorld& world : resolved) {
        std::filesystem::path path(world.scenePath);
        if (path.is_relative() && !assetRoot.empty()) {
            path = std::filesystem::path(assetRoot) / path;
        }
        world.scenePath = path.lexically_normal().string();
    }
    return resolved;
}

const GameWorld* findWorld(
    const std::vector<GameWorld>& worlds,
    std::string_view id) noexcept
{
    for (const GameWorld& world : worlds) {
        if (world.id == id) {
            return &world;
        }
    }
    return nullptr;
}

} // namespace

bool validateGameProject(const GameProject& project, std::string& error)
{
    error.clear();
    if (project.id.empty()) {
        error = "Game project id must not be empty";
        return false;
    }
    if (project.displayName.empty()) {
        error = "Game display name must not be empty";
        return false;
    }
    if (project.width == 0 || project.height == 0 || project.targetFPS <= 0.0f) {
        error = "Window size and target FPS must be positive";
        return false;
    }
    if (project.serverMode && project.worlds.empty() &&
        project.startupWorld.empty()) {
        return true;
    }
    if (project.worlds.empty()) {
        error = "A client game must declare at least one World";
        return false;
    }
    if (project.startupWorld.empty()) {
        error = "A client game must select startupWorld";
        return false;
    }

    std::unordered_set<std::string> ids;
    for (const GameWorld& world : project.worlds) {
        if (world.id.empty() || world.scenePath.empty()) {
            error = "Every World needs a stable id and scenePath";
            return false;
        }
        if (!ids.insert(world.id).second) {
            error = "Duplicate World id: " + world.id;
            return false;
        }
    }
    if (findWorld(project.worlds, project.startupWorld) == nullptr) {
        error = "startupWorld is not present in worlds: " +
                project.startupWorld;
        return false;
    }
    return true;
}

IGameWorldRouter* gameWorldRouter(IEngineHost& host) noexcept
{
    try {
        return static_cast<IGameWorldRouter*>(
            host.findService(kHostServiceGameWorldRouter));
    } catch (...) {
        return nullptr;
    }
}

int runGameProject(GameProject project, AppCommandLine commandLine)
{
    // Command-line server mode selects the server module graph too. Apply it
    // before validation and before deciding whether to add the World router.
    project.serverMode = project.serverMode || commandLine.server;

    std::string error;
    if (!validateGameProject(project, error)) {
        std::fprintf(stderr, "[GameProject] %s\n", error.c_str());
        return static_cast<int>(AppException::Code::ConfigError);
    }
    if (commandLine.help) {
        commandLine.printHelp(project.displayName.c_str());
        return 0;
    }
    if (commandLine.version) {
        commandLine.printVersion(
            project.displayName.c_str(), project.version.c_str());
        return 0;
    }

    if (!commandLine.assetRoot.empty()) {
        project.assetRoot = commandLine.assetRoot;
    }
    project.worlds = resolveWorldPaths(project.worlds, project.assetRoot);

    if (!commandLine.scenePath.empty() && !project.serverMode) {
        project.startupWorld = "__command_line__";
        project.worlds.push_back(GameWorld{
            .id = project.startupWorld,
            .scenePath = commandLine.scenePath,
            .sceneName = "CommandLineWorld",
        });
    }

    const GameWorld* startup = project.serverMode
        ? nullptr
        : findWorld(project.worlds, project.startupWorld);

    GameDesc desc;
    desc.name = project.displayName.c_str();
    desc.width = project.width;
    desc.height = project.height;
    desc.targetFPS = project.targetFPS;
    desc.enableRenderThread = project.enableRenderThread;
    desc.assetRoot = project.assetRoot.c_str();
    desc.userDataPath = project.userDataPath.c_str();
    desc.scenePath = startup == nullptr ? "" : startup->scenePath.c_str();
    desc.enablePresentation = project.enablePresentation;
    desc.enablePhysics = project.enablePhysics;
    desc.serverMode = project.serverMode;

    auto configureGameModules = project.configureModules;
    if (!project.serverMode) {
        auto worlds = project.worlds;
        auto startupWorld = project.startupWorld;
        desc.configureModules = [
            worlds = std::move(worlds),
            startupWorld = std::move(startupWorld),
            configureGameModules = std::move(configureGameModules)](
                EngineModuleRuntime& runtime) mutable {
            auto result = runtime.modules().emplace<GameWorldRouterModule>(
                runtime.context().host(),
                std::move(worlds),
                std::move(startupWorld));
            if (!result || !configureGameModules) {
                return result;
            }
            return configureGameModules(runtime);
        };
    } else {
        desc.configureModules = std::move(configureGameModules);
    }

    try {
        auto application = IApplication::create(desc, commandLine);
        application->run();
        return 0;
    } catch (const AppException& exception) {
        std::fprintf(stderr, "[GameProject] %s\n", exception.what());
        return static_cast<int>(exception.getCode());
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[GameProject] %s\n", exception.what());
        return static_cast<int>(AppException::Code::Unknown);
    }
}

int runGameProject(GameProject project, int argc, char* argv[])
{
    return runGameProject(
        std::move(project),
        AppCommandLine::parse(argc, argv));
}

} // namespace ayt::app

#include <AYApplication/GameProject.h>

#include <AYApplication/EngineModuleContext.h>
#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/GameFlowAssets.h>
#include <AYApplication/GameFlowRuntimeModule.h>
#include <AYApplication/GameFlowWorldActions.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RuntimeSceneLoader.h>
#include <AYApplication/RuntimeSceneLoaderModule.h>
#include <AYGameLoop/SubSystemModule.h>
#include <AYScene.h>

#include <cctype>
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

bool endsWithCaseInsensitive(
    std::string_view value,
    std::string_view suffix) noexcept
{
    if (value.size() < suffix.size()) return false;
    const std::string_view tail = value.substr(value.size() - suffix.size());
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(tail[index]))
            != std::tolower(static_cast<unsigned char>(suffix[index]))) {
            return false;
        }
    }
    return true;
}

bool hasWindowsDrivePrefix(std::string_view value) noexcept
{
    if (value.size() < 2 || value[1] != ':') return false;
    const char letter = value.front();
    return (letter >= 'A' && letter <= 'Z')
        || (letter >= 'a' && letter <= 'z');
}

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

        if (_startupWorld.empty()) {
            _currentWorld.clear();
            _lastError.clear();
            return true;
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
                static_cast<void*>(installedSubSystem())) {
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

std::string resolveAssetPath(
    std::string pathValue,
    std::string_view assetRoot)
{
    std::filesystem::path path(std::move(pathValue));
    if (path.is_relative() && !assetRoot.empty()) {
        path = std::filesystem::path(assetRoot) / path;
    }
    return path.lexically_normal().string();
}

bool normalizeFlowAssetPath(
    std::string_view pathValue,
    std::string& normalized,
    std::string& error)
{
    if (hasWindowsDrivePrefix(pathValue)) {
        error = "startupFlow must be relative to assetRoot";
        return false;
    }
    if (pathValue.find('\\') != std::string_view::npos) {
        error = "startupFlow must use portable forward-slash separators";
        return false;
    }
    const std::filesystem::path supplied(pathValue);
    if (supplied.empty() || supplied.is_absolute()
        || supplied.has_root_name() || supplied.has_root_directory()) {
        error = "startupFlow must be relative to assetRoot";
        return false;
    }
    const std::filesystem::path path = supplied.lexically_normal();
    if (path.empty() || path == ".") {
        error = "startupFlow must not be empty";
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            error = "startupFlow must stay inside assetRoot";
            return false;
        }
    }
    if (!endsWithCaseInsensitive(
            path.filename().string(), ".gameflow.json")) {
        error = "startupFlow must reference a .gameflow.json asset";
        return false;
    }
    normalized = path.generic_string();
    error.clear();
    return true;
}

bool resolveFlowAssetPath(
    std::string_view pathValue,
    std::string_view assetRoot,
    std::string& resolved,
    std::string& error)
{
    std::string normalized;
    if (!normalizeFlowAssetPath(pathValue, normalized, error)) return false;
    resolved = (std::filesystem::path(assetRoot)
                / std::filesystem::path(normalized))
                   .lexically_normal().string();
    return true;
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

bool validateGameFlowWorldReferences(
    const GameFlowProgram& program,
    const std::vector<GameWorld>& worlds,
    std::string& error)
{
    for (const auto& [flowId, plan] : program.plans) {
        const GameFlowDocument& document = plan.document;
        for (const auto& transition : document.transitions) {
            for (std::size_t actionIndex = 0;
                 actionIndex < transition.actions.size();
                 ++actionIndex) {
                const auto& action = transition.actions[actionIndex];
                if (action.action != kGameFlowActionWorldReplace) continue;
                const auto argument = action.arguments.find("worldId");
                const auto* worldId = argument == action.arguments.end()
                    ? nullptr
                    : std::get_if<std::string>(&argument->second.data);
                if (worldId == nullptr
                    || findWorld(worlds, *worldId) == nullptr) {
                    error = "GameFlow '" + flowId + "' transition '"
                        + transition.id + "' action["
                        + std::to_string(actionIndex)
                        + "] references an unknown World id: "
                        + (worldId == nullptr ? std::string("<invalid>")
                                              : *worldId);
                    return false;
                }
            }
        }
    }
    return true;
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
        project.startupWorld.empty() && project.startupFlow.empty()) {
        return true;
    }
    if (!project.serverMode && project.worlds.empty()) {
        error = "A client game must declare at least one World";
        return false;
    }
    if (!project.serverMode && project.startupWorld.empty()
        && project.startupFlow.empty()) {
        error = "A client game must select startupFlow or startupWorld";
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
    if (!project.startupWorld.empty()
        && findWorld(project.worlds, project.startupWorld) == nullptr) {
        error = "startupWorld is not present in worlds: " +
                project.startupWorld;
        return false;
    }
    if (!project.startupFlow.empty()) {
        std::string normalized;
        if (!normalizeFlowAssetPath(
                project.startupFlow, normalized, error)) return false;
    }
    return true;
}

bool resolveGameProjectStartup(
    const GameProject& project,
    const AppCommandLine& commandLine,
    GameProjectStartupSelection& selection,
    std::string& error)
{
    selection = {};
    error.clear();
    const bool serverMode = project.serverMode || commandLine.server;
    const std::string_view assetRoot = commandLine.assetRoot.empty()
        ? std::string_view(project.assetRoot)
        : std::string_view(commandLine.assetRoot);

    // Direct Scene launch is the strongest client debugging override and
    // intentionally bypasses all GameFlow startup behavior.
    if (!serverMode && !commandLine.scenePath.empty()) {
        selection.source = GameProjectStartupSource::CommandLineScene;
        selection.scenePath = commandLine.scenePath;
        selection.worldId = "__command_line__";
        return true;
    }
    if (!commandLine.flowPath.empty()) {
        selection.source = GameProjectStartupSource::CommandLineFlow;
        return resolveFlowAssetPath(
            commandLine.flowPath, assetRoot, selection.flowPath, error);
    }
    if (!project.startupFlow.empty()) {
        selection.source = GameProjectStartupSource::ProjectFlow;
        return resolveFlowAssetPath(
            project.startupFlow, assetRoot, selection.flowPath, error);
    }
    if (serverMode) return true;
    if (!project.startupWorld.empty()) {
        const GameWorld* world = findWorld(project.worlds, project.startupWorld);
        if (world == nullptr) {
            error = "startupWorld is not present in worlds: "
                + project.startupWorld;
            return false;
        }
        selection.source = GameProjectStartupSource::ProjectWorld;
        selection.worldId = world->id;
        selection.scenePath = resolveAssetPath(world->scenePath, assetRoot);
        return true;
    }
    error = "No startupFlow or startupWorld was selected";
    return false;
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

    GameProjectStartupSelection startup;
    std::string error;
    if (!resolveGameProjectStartup(project, commandLine, startup, error)) {
        std::fprintf(stderr, "[GameProject] %s\n", error.c_str());
        return static_cast<int>(AppException::Code::ConfigError);
    }
    if (startup.source == GameProjectStartupSource::CommandLineScene) {
        project.startupWorld = "__command_line__";
        project.startupFlow.clear();
        project.worlds.push_back(GameWorld{
            .id = project.startupWorld,
            .scenePath = startup.scenePath,
            .sceneName = "CommandLineWorld",
        });
    } else if (startup.source == GameProjectStartupSource::CommandLineFlow) {
        // Keep the validated project representation asset-relative. The
        // resolved path in startup is diagnostic/output data only; runtime
        // selects the root from the canonical asset catalog below.
        project.startupFlow = commandLine.flowPath;
    }

    if (!validateGameProject(project, error)) {
        std::fprintf(stderr, "[GameProject] %s\n", error.c_str());
        return static_cast<int>(AppException::Code::ConfigError);
    }
    project.worlds = resolveWorldPaths(project.worlds, project.assetRoot);

    const bool usesGameFlow =
        startup.source == GameProjectStartupSource::CommandLineFlow
        || startup.source == GameProjectStartupSource::ProjectFlow;
    std::unique_ptr<GameFlowRuntimePreparation> preparedGameFlow;
    if (usesGameFlow) {
        auto flowAssets = std::make_shared<GameFlowAssetCatalog>();
        if (!scanGameFlowAssets(project.assetRoot, *flowAssets, &error)) {
            std::fprintf(stderr, "[GameProject] %s\n", error.c_str());
            return static_cast<int>(AppException::Code::ConfigError);
        }
        const std::string& startupFlowAsset =
            startup.source == GameProjectStartupSource::CommandLineFlow
            ? commandLine.flowPath : project.startupFlow;
        const GameFlowAssetDocument* rootFlow =
            flowAssets->findByAssetPath(startupFlowAsset);
        if (rootFlow == nullptr) {
            std::fprintf(stderr,
                "[GameProject] startupFlow is missing, invalid, or resolves "
                "outside assetRoot: %s\n", startupFlowAsset.c_str());
            return static_cast<int>(AppException::Code::ConfigError);
        }
        GameFlowRuntimeConfig config;
        config.documentPath = rootFlow->absolutePath;
        config.configureRegistry = std::move(project.configureGameFlow);
        config.enableWorldActions = !project.serverMode;
        config.resolveDocument = makeGameFlowAssetResolver(
            std::move(flowAssets));
        preparedGameFlow = prepareGameFlowRuntime(
            std::move(config), &error);
        if (!preparedGameFlow) {
            std::fprintf(stderr, "[GameProject] %s\n", error.c_str());
            return static_cast<int>(AppException::Code::ConfigError);
        }
        if (!project.serverMode
            && !validateGameFlowWorldReferences(
                preparedGameFlow->program(), project.worlds, error)) {
            std::fprintf(stderr, "[GameProject] %s\n", error.c_str());
            return static_cast<int>(AppException::Code::ConfigError);
        }
    }
    const GameWorld* startupWorld = project.serverMode || usesGameFlow
        ? nullptr : findWorld(project.worlds, project.startupWorld);

    // Package smoke checks exercise the real executable, dynamic-library
    // loading, command-line parser, project contract and startup GameFlow
    // preparation without relying on an interactive desktop in CI.
    if (commandLine.validateStartup) return 0;

    GameDesc desc;
    desc.name = project.displayName.c_str();
    desc.width = project.width;
    desc.height = project.height;
    desc.targetFPS = project.targetFPS;
    desc.enableRenderThread = project.enableRenderThread;
    desc.assetRoot = project.assetRoot.c_str();
    desc.userDataPath = project.userDataPath.c_str();
    desc.scenePath = startupWorld == nullptr
        ? "" : startupWorld->scenePath.c_str();
    desc.enablePresentation = project.enablePresentation;
    desc.enablePhysics = project.enablePhysics;
    desc.serverMode = project.serverMode;

    auto configureGameModules = project.configureModules;
    auto preparedGameFlowSeed =
        std::make_shared<std::unique_ptr<GameFlowRuntimePreparation>>(
            std::move(preparedGameFlow));
    if (!project.serverMode) {
        auto worlds = project.worlds;
        auto routerStartupWorld = usesGameFlow
            ? std::string{} : project.startupWorld;
        desc.configureModules = [
            worlds = std::move(worlds),
            routerStartupWorld = std::move(routerStartupWorld),
            preparedGameFlowSeed,
            configureGameModules = std::move(configureGameModules)](
                EngineModuleRuntime& runtime) mutable {
            auto result = runtime.modules().emplace<GameWorldRouterModule>(
                runtime.context().host(),
                std::move(worlds),
                std::move(routerStartupWorld));
            if (!result) return result;
            if (*preparedGameFlowSeed) {
                result = runtime.modules().emplace<GameFlowRuntimeModule>(
                    runtime.context().host(),
                    std::move(*preparedGameFlowSeed));
                if (!result) return result;
            }
            if (!configureGameModules) return result;
            return configureGameModules(runtime);
        };
    } else {
        desc.configureModules = [
            preparedGameFlowSeed,
            configureGameModules = std::move(configureGameModules)](
                EngineModuleRuntime& runtime) mutable {
            ayt::module::ModuleResult result =
                ayt::module::ModuleResult::success();
            if (*preparedGameFlowSeed) {
                result = runtime.modules().emplace<GameFlowRuntimeModule>(
                    runtime.context().host(),
                    std::move(*preparedGameFlowSeed));
                if (!result) return result;
            }
            if (!configureGameModules) return result;
            return configureGameModules(runtime);
        };
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

int runGameProject(GameProject project, int argc, wchar_t* argv[])
{
    return runGameProject(
        std::move(project),
        AppCommandLine::parse(argc, argv));
}

} // namespace ayt::app

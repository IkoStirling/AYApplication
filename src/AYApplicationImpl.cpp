// AYApplicationImpl.cpp - application implementation

#include <AYApplication.h>
#include <AYGameLoop.h>
#include <AYCore.h>

#include <AYRegisterDefaultModules.h>
#include <IEngineHost.h>

#include <AYDeviceSubSystem.h>
#include <AYDeviceInputProvider.h>
#include <AYEntityModule.h>
#include <AYScene.h>
#include <AYSceneManager.h>
#include <AYScriptSubSystem.h>
#include <AYSubSystemRegistry.h>

#include <ayevent/EventBus.h>
#include <AYAppEventHost.h>

#include <cstdio>
#include <memory>

namespace ayt::app
{

AppException::AppException(Code code, const char* message)
    : _code(code), _message(message) {}

AppException::AppException(Code code, const std::string& message)
    : _code(code), _message(message) {}

const char* AppException::what() const noexcept
{
    return _message.c_str();
}

AppCommandLine AppCommandLine::parse(int argc, char* argv[])
{
    AppCommandLine cmd;
    cmd.args.assign(argv, argv + argc);

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-help" || arg == "--help") {
            cmd.help = true;
        } else if (arg == "-version" || arg == "--version") {
            cmd.version = true;
        } else if (arg == "-debug") {
            cmd.debug = true;
        } else if (arg == "-no-audio") {
            cmd.noAudio = true;
        } else if (arg == "-no-physics") {
            cmd.noPhysics = true;
        } else if (arg == "-server") {
            cmd.server = true;
        } else if (arg == "-width" && i + 1 < argc) {
            cmd.width = std::stoul(argv[++i]);
        } else if (arg == "-height" && i + 1 < argc) {
            cmd.height = std::stoul(argv[++i]);
        } else if (arg == "-fps" && i + 1 < argc) {
            cmd.fps = std::stof(argv[++i]);
        } else if (arg == "-log" && i + 1 < argc) {
            cmd.logLevel = argv[++i];
        } else if (arg == "-config" && i + 1 < argc) {
            cmd.configFile = argv[++i];
        } else if (arg == "-asset-root" && i + 1 < argc) {
            cmd.assetRoot = argv[++i];
        } else if (arg == "-user-data" && i + 1 < argc) {
            cmd.userDataPath = argv[++i];
        } else if (arg == "-scene" && i + 1 < argc) {
            cmd.scenePath = argv[++i];
        } else {
            cmd.unknownArgs.push_back(argv[i]);
        }
    }

    return cmd;
}

void AppCommandLine::printHelp(const char* appName) const
{
    std::printf("Usage: %s [options]\n", appName);
    std::printf("Options:\n");
    std::printf("  -help, --help          Show this help message\n");
    std::printf("  -version, --version    Show version information\n");
    std::printf("  -debug                 Enable debug mode\n");
    std::printf("  -width <n>             Set window width\n");
    std::printf("  -height <n>            Set window height\n");
    std::printf("  -fps <n>               Set target FPS\n");
    std::printf("  -log <level>           Set log level (trace, debug, info, warn, error)\n");
    std::printf("  -config <path>         Set config file path\n");
    std::printf("  -asset-root <path>     Set asset root directory\n");
    std::printf("  -user-data <path>      Set user data directory\n");
    std::printf("  -scene <path>          Load startup .ayscene (Client)\n");
    std::printf("  -server                Headless Server assembly (no Device/Audio/Renderer)\n");
    std::printf("  -no-audio              Disable audio system\n");
    std::printf("  -no-physics            Disable PhysicsSubSystem\n");
}

void AppCommandLine::printVersion(const char* appName, const char* version) const
{
    std::printf("%s v%s\n", appName, version);
    std::printf("Engine: %s\n", "AYEngine");
}

class ApplicationImpl : public IApplication {
public:
    explicit ApplicationImpl(const GameDesc& desc) : _desc(desc) {}

    explicit ApplicationImpl(const GameDesc& desc, const AppCommandLine& cmdLine)
        : _desc(desc), _cmdLine(cmdLine)
    {
        if (_cmdLine.width > 0) {
            _desc.width = _cmdLine.width;
        }
        if (_cmdLine.height > 0) {
            _desc.height = _cmdLine.height;
        }
        if (_cmdLine.fps > 0.0f) {
            _desc.targetFPS = _cmdLine.fps;
        }
        if (!_cmdLine.logLevel.empty()) {
            _desc.logLevel = _cmdLine.logLevel.c_str();
        }
        if (!_cmdLine.configFile.empty()) {
            _desc.configFile = _cmdLine.configFile.c_str();
        }
        if (!_cmdLine.assetRoot.empty()) {
            _desc.assetRoot = _cmdLine.assetRoot.c_str();
        }
        if (!_cmdLine.userDataPath.empty()) {
            _desc.userDataPath = _cmdLine.userDataPath.c_str();
        }
        if (!_cmdLine.scenePath.empty()) {
            _desc.scenePath = _cmdLine.scenePath.c_str();
        }
        if (_cmdLine.server) {
            _desc.serverMode = true;
        }
        if (_cmdLine.noPhysics) {
            _desc.enablePhysics = false;
        }
    }

    const GameDesc& getDesc() const override { return _desc; }

    ayt::game::GameLoop& getGameLoop() override { return ayt::game::GameLoop::instance(); }

    const AppCommandLine& getCommandLine() const override { return _cmdLine; }

    const char* getVersion() const override { return "1.0.0"; }
    const char* getEngineVersion() const override { return "1.0.0"; }

    void registerSubSystems() override
    {
        const bool enablePhysics = _desc.enablePhysics && !_cmdLine.noPhysics;

        if (_desc.serverMode || _cmdLine.server) {
            // Engine-host §2.3 Server assembly.
            ServerModuleOptions opts;
            opts.enableScript = true;
            opts.enablePhysics = enablePhysics;
            registerDefaultServerModules(opts);
            bindBuiltinHostServices(engineHost());
            return;
        }

        // Engine-host Step 1: shared Client assembly table
        // (docs/engine-host.md §2.1).
        ClientModuleOptions opts;
        opts.windowTitle = _desc.name;
        opts.windowWidth = static_cast<int>(_desc.width);
        opts.windowHeight = static_cast<int>(_desc.height);
        opts.enableAudio = !_cmdLine.noAudio;
        opts.enablePresentation = _desc.enablePresentation;
        opts.enablePhysics = enablePhysics;
        registerDefaultClientModules(opts);
        bindBuiltinHostServices(engineHost());

        // INT-02: Script ← DeviceInputProvider. Lifetime: static provider
        // points at DeviceSubSystem's manager; ScriptSubSystem::shutdown
        // clears the provider before Device is destroyed.
        if (auto* devSub = ayt::device::DeviceSubSystem::findRegistered()) {
            auto* sub = engineHost().findSubSystem("ayt.script.runtime");
            if (auto* scriptSub =
                    dynamic_cast<ayt::script::ScriptSubSystem*>(sub)) {
                static ayt::device::DeviceInputProvider s_provider(
                    &devSub->manager());
                scriptSub->bridge().setInputProvider(&s_provider);
            }
        }
    }

    void run() override
    {
        EngineHostScope hostScope(defaultEngineHost());

        auto& loop = ayt::game::GameLoop::instance();
        loop.setTargetFPS(_desc.targetFPS);
        loop.setRenderThreadEnabled(_desc.enableRenderThread);

        onInit();
        registerSubSystems();
        bootstrapClientPlayScene();
        loop.run();

        onPreShutdown();
        teardownClientPlayScene();
        loop.shutdown();
        // Phase 4 (a8c8be9) lesson applied: the host application, not
        // GameLoop, owns the per-instance EventBus listener cleanup.
        // Subscribers created via `_events.subscribe<T>(...)` (or via
        // ayt::event::EventBus::instance() through eventBus()) are released
        // here, LIFO, before the rest of the application finishes teardown.
        _events.disconnect();
        onShutdown();
    }

    ayt::event::EventBus& eventBus() override
    {
        return ayt::event::EventBus::instance();
    }

    IEngineHost& engineHost() override
    {
        return defaultEngineHost();
    }

private:
    /// P0 Client: own a Play Scene, optionally load `.ayscene`, setCurrent so
    /// World::instance() / EntitySubSystem tick the Scene World (not the
    /// process fallback). Re-bootstrap systems onto the active World when
    /// presentation is enabled (first bootstrap may have targeted fallback).
    void bootstrapClientPlayScene()
    {
        auto* sm = engineHost().scenes();
        if (sm == nullptr) {
            return;
        }

        _clientScene = std::make_unique<ayt::scene::Scene>(
            ayt::scene::SceneMode::Play,
            _desc.name ? _desc.name : "Client");

        if (_desc.scenePath != nullptr && _desc.scenePath[0] != '\0') {
            ayt::serializer::SerializeError err{};
            if (!_clientScene->load(_desc.scenePath, &err)) {
                std::fprintf(stderr,
                    "[Application] failed to load scene '%s'\n",
                    _desc.scenePath);
            }
        }

        sm->setCurrent(_clientScene.get());

        if (_desc.enablePresentation) {
            // Systems register via World::instance() → now the Play Scene.
            ayt::entity::bootstrapModule();
        }
    }

    void teardownClientPlayScene()
    {
        if (auto* sm = engineHost().scenes()) {
            if (sm->current() == _clientScene.get()) {
                sm->setCurrent(nullptr);
            }
        }
        _clientScene.reset();
    }

    GameDesc       _desc;
    AppCommandLine _cmdLine;
    std::unique_ptr<ayt::scene::Scene> _clientScene;

    // Host-owned EventBus subscriptions (Phase 4 §a8c8be9 lesson).
    // Connect listeners here instead of holding raw ScopedConnection
    // members so that disconnect() in run() releases them all in one place.
    ayt::app::EventBusHostScope _events;
};

std::unique_ptr<IApplication> IApplication::create(const GameDesc& desc)
{
    return std::make_unique<ApplicationImpl>(desc);
}

std::unique_ptr<IApplication> IApplication::create(const GameDesc& desc,
                                                   const AppCommandLine& cmdLine)
{
    return std::make_unique<ApplicationImpl>(desc, cmdLine);
}

IEngineHost& IApplication::engineHost()
{
    return defaultEngineHost();
}

} // namespace ayt::app

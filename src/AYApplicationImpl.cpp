// AYApplicationImpl.cpp - application implementation

#include <AYApplication.h>
#include <AYGameLoop.h>
#include <AYCore.h>

#include <AYAudioSubSystem.h>
#include <AYAudioBackendFactory.h>

#include <AYDeviceSubSystem.h>

#include <cstdio>

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
    std::printf("  -no-audio              Disable audio system\n");
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
    }

    const GameDesc& getDesc() const override { return _desc; }

    ayt::game::GameLoop& getGameLoop() override { return ayt::game::GameLoop::instance(); }

    const AppCommandLine& getCommandLine() const override { return _cmdLine; }

    const char* getVersion() const override { return "1.0.0"; }
    const char* getEngineVersion() const override { return "1.0.0"; }

    // Register audio subsystem with a backend chosen from CLI flags:
    //   -no-audio   → Null backend (commands flow but no device opens)
    //   (default)   → Miniaudio backend (real device)
    void registerSubSystems() override
    {
        registerDeviceSubSystem();

        // Audio is a presentation module — Server builds skip it (matches
        // AYAudio/design.md §1.2 non-goal "Run on Server build"). Until the
        // build-target flag infra lands, we treat `noAudio` as the only gate.
        if (_cmdLine.noAudio) {
            return;
        }

        auto audioSub = std::make_unique<ayt::audio::AudioSubSystem>();
        audioSub->setBackend(ayt::audio::makeMiniaudioBackend());
        // Ownership transfers to the GameLoop / SubSystemRegistry, which
        // deletes via the ISubSystem base pointer on shutdown.
        ayt::game::GameLoop::instance().registerSubSystem(audioSub.release());
    }

    // Create the window + input devices and register "Device" (priority 0, so
    // it initializes and polls first). The window it owns is exposed to the
    // renderer via RendererSubSystem::setWindowProvider(makeWindowProvider())
    // — wired by the client that also registers the renderer, keeping this
    // module free of the render/bgfx dependency.
    void registerDeviceSubSystem()
    {
        ayt::device::DeviceConfig config{};
        config.window.title  = _desc.name;
        config.window.width  = static_cast<int>(_desc.width);
        config.window.height = static_cast<int>(_desc.height);
        ayt::device::DeviceSubSystem::setBootstrapConfig(config);
        ayt::device::DeviceSubSystem::registerSubSystem();
    }

    void run() override
    {
        auto& loop = ayt::game::GameLoop::instance();
        loop.setTargetFPS(_desc.targetFPS);
        loop.setRenderThreadEnabled(_desc.enableRenderThread);

        onInit();
        registerSubSystems();
        loop.run();

        onPreShutdown();
        loop.shutdown();
        onShutdown();
    }

private:
    GameDesc       _desc;
    AppCommandLine _cmdLine;
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

} // namespace ayt::app

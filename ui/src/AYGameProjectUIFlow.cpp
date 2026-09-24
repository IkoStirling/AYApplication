#include <AYApplication/GameProjectUIFlow.h>

#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIFlowRuntimeModule.h>
#include <AYApplication/UIManagerFlowScreenHost.h>
#include <AYDevice/DeviceRuntimeModule.h>
#include <AYDevice/DeviceSubSystem.h>
#include <AYEventSystem/Events/WindowEvents.h>
#include <AYEventSystem/SubscriptionScope.h>
#include <AYGameLoop.h>
#include <AYRenderer/RendererRuntimeModule.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYUI/DeviceInputBridge.h>
#include <AYUI/Theme.h>
#include <AYUI/UIFlow.h>
#include <AYUI/UIManager.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ayt::app
{
namespace
{

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string diagnosticText(
    const std::vector<ayt::ui::UIFlowDiagnostic>& diagnostics)
{
    std::string result;
    for (const auto& diagnostic : diagnostics) {
        if (!result.empty()) result += " | ";
        if (!diagnostic.path.empty()) {
            result += diagnostic.path;
            result += ": ";
        }
        result += diagnostic.message;
    }
    return result;
}

class GameProjectUIFlowSubSystem final : public ayt::game::ISubSystem
{
public:
    GameProjectUIFlowSubSystem(
        IEngineHost& host,
        std::string assetRoot,
        GameProjectUIFlowConfig config)
        : _host(host),
          _assetRoot(std::move(assetRoot)),
          _config(std::move(config)),
          _screenHost(_manager, uiAssetRoot().string()),
          _runtime(_screenHost),
          _input(_manager)
    {
    }

    const char* getName() const override
    {
        return kUIFlowRuntimeSubSystemName.data();
    }

    const ayt::game::SubSystemDescriptor& getDescriptor() const override
    {
        static const ayt::game::SubSystemDescriptor descriptor{
            .name = kUIFlowRuntimeSubSystemName.data(),
            .dependencies = {"Device", "Renderer"},
            .basePriority = 100,
            .timeType = ayt::game::SubSystemDescriptor::TimeType::Unscaled,
            .phases = ayt::game::phaseBit(
                ayt::game::FramePhase::Presentation),
            .clock = ayt::game::ClockDomain::Unscaled,
            .initializeAfter = {"Device", "Renderer"},
            .phasePriority = 100,
            .reads = {"Application.UIFlowCommands", "Device.Input"},
            .writes = {"Application.UIFlowPresentation"},
        };
        return descriptor;
    }

    bool initialize() override
    {
        _renderer = ayt::render::RendererSubSystem::findRegistered();
        _device = ayt::device::DeviceSubSystem::findRegistered();
        if (_renderer == nullptr || !_renderer->isReady()
            || _device == nullptr || !_device->isReady()) {
            return fail("Renderer or Device runtime is unavailable.");
        }

        const std::filesystem::path flowPath =
            (std::filesystem::path(_assetRoot) / _config.flowPath)
                .lexically_normal();
        const std::string encoded = readTextFile(flowPath);
        if (encoded.empty()) {
            return fail("Cannot read UIFlow asset: " + flowPath.string());
        }

        ayt::ui::UIFlowDocument document;
        std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
        if (!ayt::ui::UIFlowSerializer::deserialize(
                encoded, document, &diagnostics)) {
            return fail("Cannot parse UIFlow asset: "
                + diagnosticText(diagnostics));
        }

        if (!_backend.initialize(_renderer->renderer())) {
            return fail("Cannot initialize the UI render backend.");
        }

        int width = 0;
        int height = 0;
        _device->manager().window().getSize(width, height);
        if (width <= 0 || height <= 0) {
            return fail("The client window has no drawable size.");
        }

        _manager.initialize(&_backend);
        _manager.setClientSize(
            static_cast<float>(width), static_cast<float>(height));
        ayt::ui::ThemeManager::get().ensureDefaultThemes();
        ayt::ui::ThemeManager::get().setActiveTheme("dark");

        _input.connect(_device->manager());
        _input.bindTextInputFocus(_manager);
        _events.subscribe<ayt::event::WindowResizeEvent>(
            [this](const ayt::event::WindowResizeEvent& event) {
                if (event.width <= 0 || event.height <= 0) return;
                _manager.setClientSize(
                    static_cast<float>(event.width),
                    static_cast<float>(event.height));
                _backend.setFramebufferSize(
                    static_cast<std::uint16_t>(event.width),
                    static_cast<std::uint16_t>(event.height));
            });

        std::string error;
        if (!_runtime.load(std::move(document), &error)
            || !_runtime.start(_config.entry, &error)) {
            shutdownPresentation();
            return fail("Cannot start UIFlow: " + error);
        }

        ayt::game::GameLoop::instance().setRenderCallback([this]() {
            if (_renderer == nullptr || !_initialized) return;
            _renderer->renderCompositeFrame(
                true, &_backend,
                [this](bool, ayt::render::CompositeUiPhase phase) {
                    if (phase == ayt::render::CompositeUiPhase::Populate) {
                        _manager.populateFrame();
                    } else {
                        _manager.flushFrame();
                    }
                });
        });
        _initialized = true;
        _lastError.clear();
        return true;
    }

    void update(float deltaTime) override
    {
        if (!_initialized) return;
        _screenHost.update(deltaTime);
        _manager.update(deltaTime);
        _manager.layout();
    }

    void fixedUpdate(float) override {}

    void shutdown() override
    {
        if (!_initialized && !_backend.isInitialized()) return;
        ayt::game::GameLoop::instance().setRenderCallback({});
        _runtime.unload();
        shutdownPresentation();
        _initialized = false;
        _renderer = nullptr;
        _device = nullptr;
    }

    UIFlowRuntime& runtime() noexcept { return _runtime; }

private:
    std::filesystem::path uiAssetRoot() const
    {
        return std::filesystem::path(_assetRoot).lexically_normal();
    }

    bool fail(std::string message)
    {
        _lastError = std::move(message);
        std::fprintf(stderr, "[GameProjectUIFlow] %s\n", _lastError.c_str());
        return false;
    }

    void shutdownPresentation() noexcept
    {
        _events.disconnect();
        _input.disconnect();
        _manager.shutdown();
        _backend.shutdown();
    }

    IEngineHost& _host;
    std::string _assetRoot;
    GameProjectUIFlowConfig _config;
    ayt::render::RendererSubSystem* _renderer = nullptr;
    ayt::device::DeviceSubSystem* _device = nullptr;
    ayt::render::UIRenderBackend _backend;
    ayt::ui::UIManager _manager;
    UIManagerFlowScreenHost _screenHost;
    UIFlowRuntime _runtime;
    ayt::ui::DeviceInputBridge _input;
    ayt::event::SubscriptionScope _events;
    std::string _lastError;
    bool _initialized = false;
};

struct GameProjectUIFlowSeed
{
    IEngineHost* host = nullptr;
    std::string assetRoot;
    GameProjectUIFlowConfig config;
};

class GameProjectUIFlowModule final : public ayt::game::SubSystemModule
{
public:
    GameProjectUIFlowModule(
        IEngineHost& host,
        std::string assetRoot,
        GameProjectUIFlowConfig config)
        : SubSystemModule(
              ayt::module::ModuleDescriptor{
                  .id = std::string(kUIFlowRuntimeModuleId),
                  .displayName = "Game Project UI Flow",
                  .version = "1.0.0",
                  .dependencies = {
                      ayt::module::ModuleDependency::required(
                          std::string(ayt::device::kDeviceRuntimeModuleId)),
                      ayt::module::ModuleDependency::required(
                          std::string(ayt::render::kRendererRuntimeModuleId)),
                  }},
              std::string(kUIFlowRuntimeSubSystemName),
              [seed = std::make_shared<GameProjectUIFlowSeed>(
                   GameProjectUIFlowSeed{
                       &host, std::move(assetRoot), std::move(config)})]() mutable {
                  return std::make_unique<GameProjectUIFlowSubSystem>(
                      *seed->host,
                      std::move(seed->assetRoot),
                      std::move(seed->config));
              },
              [&host](ayt::module::IModuleContext&,
                      ayt::game::ISubSystem& system) {
                  auto* typed =
                      dynamic_cast<GameProjectUIFlowSubSystem*>(&system);
                  if (typed == nullptr) {
                      return ayt::module::ModuleResult::failure(
                          ayt::module::ModuleErrorCode::InstallationFailed,
                          "GameProject UIFlow module received an incompatible subsystem");
                  }
                  host.provide(kHostServiceUIFlowRuntime, &typed->runtime());
                  return ayt::module::ModuleResult::success();
              }),
          _host(host)
    {
    }

    void shutdown(ayt::module::IModuleContext& context) noexcept override
    {
        try {
            _host.provideService(kHostServiceUIFlowRuntime, nullptr);
        } catch (...) {
        }
        SubSystemModule::shutdown(context);
    }

private:
    IEngineHost& _host;
};

} // namespace

void enableGameProjectUIFlow(
    GameProject& project,
    GameProjectUIFlowConfig config)
{
    auto configureModules = std::move(project.configureModules);
    const std::string assetRoot = project.assetRoot;
    project.configureModules = [
        configureModules = std::move(configureModules),
        assetRoot,
        config = std::move(config)](
            EngineModuleRuntime& runtime) mutable {
        ayt::module::ModuleResult result =
            ayt::module::ModuleResult::success();
        if (configureModules) {
            result = configureModules(runtime);
            if (!result) return result;
        }
        return runtime.modules().emplace<GameProjectUIFlowModule>(
            runtime.context().host(), assetRoot, std::move(config));
    };
}

} // namespace ayt::app

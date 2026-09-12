#include <AYApplication/GameFlowUIBridgeModule.h>

#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/GameFlowRuntimeModule.h>
#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/UIFlowRuntimeModule.h>

#include <memory>
#include <utility>

namespace ayt::app
{
namespace
{

class GameFlowUIBridgeSubSystem final : public ayt::game::ISubSystem
{
public:
    GameFlowUIBridgeSubSystem(
        IEngineHost& host,
        GameFlowRuntime& gameFlow,
        UIFlowRuntime& uiFlow,
        GameFlowUIBridgeConfig config)
        : _host(host),
          _bridge(gameFlow, uiFlow, std::move(config))
    {
    }

    ~GameFlowUIBridgeSubSystem() override
    {
        shutdown();
        try {
            if (_host.findService(kHostServiceGameFlowUIBridge) == &_bridge) {
                _host.provideService(kHostServiceGameFlowUIBridge, nullptr);
            }
        } catch (...) {
        }
    }

    const char* getName() const override
    {
        return kGameFlowUIBridgeSubSystemName.data();
    }

    const ayt::game::SubSystemDescriptor& getDescriptor() const override
    {
        static const ayt::game::SubSystemDescriptor descriptor{
            .name = kGameFlowUIBridgeSubSystemName.data(),
            .dependencies = {"GameFlowRuntime", "UIFlowRuntime"},
            .basePriority = 200,
            .timeType = ayt::game::SubSystemDescriptor::TimeType::Real,
            .phases = ayt::game::phaseBit(ayt::game::FramePhase::Ingress),
            .clock = ayt::game::ClockDomain::RealWall,
            .initializeAfter = {"GameFlowRuntime", "UIFlowRuntime"},
            .phasePriority = 200,
            .reads = {"Application.UIFlowSignal"},
            .writes = {"Application.GameFlowIntent"},
        };
        return descriptor;
    }

    bool initialize() override
    {
        return _bridge.install();
    }

    void update(float) override {}
    void fixedUpdate(float) override {}

    void shutdown() override
    {
        _bridge.uninstall();
    }

    GameFlowUIBridge& bridge() noexcept { return _bridge; }

private:
    IEngineHost& _host;
    GameFlowUIBridge _bridge;
};

struct BridgeSeed
{
    IEngineHost* host = nullptr;
    GameFlowUIBridgeConfig config;
};

} // namespace

GameFlowUIBridgeModule::GameFlowUIBridgeModule(
    IEngineHost& host,
    GameFlowUIBridgeConfig config)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kGameFlowUIBridgeModuleId),
              .displayName = "AYApplication GameFlow UI Bridge",
              .version = "1.0.0",
              .dependencies = {
                  ayt::module::ModuleDependency::required(
                      std::string(kGameFlowRuntimeModuleId)),
                  ayt::module::ModuleDependency::required(
                      std::string(kUIFlowRuntimeModuleId)),
              }},
          std::string(kGameFlowUIBridgeSubSystemName),
          [seed = std::make_shared<BridgeSeed>(
               BridgeSeed{&host, std::move(config)})]() mutable
              -> std::unique_ptr<ayt::game::ISubSystem> {
              GameFlowRuntime* gameFlow = gameFlowRuntime(*seed->host);
              UIFlowRuntime* uiFlow = uiFlowRuntime(*seed->host);
              if (gameFlow == nullptr || uiFlow == nullptr) return {};
              return std::make_unique<GameFlowUIBridgeSubSystem>(
                  *seed->host, *gameFlow, *uiFlow, std::move(seed->config));
          },
          [&host](ayt::module::IModuleContext&,
                  ayt::game::ISubSystem& system) {
              auto* typed = dynamic_cast<GameFlowUIBridgeSubSystem*>(&system);
              if (typed == nullptr) {
                  return ayt::module::ModuleResult::failure(
                      ayt::module::ModuleErrorCode::InstallationFailed,
                      "GameFlow UI bridge received an incompatible subsystem");
              }
              host.provide(kHostServiceGameFlowUIBridge, &typed->bridge());
              return ayt::module::ModuleResult::success();
          }),
      _host(host)
{
}

void GameFlowUIBridgeModule::shutdown(
    ayt::module::IModuleContext& context) noexcept
{
    try {
        _host.provideService(kHostServiceGameFlowUIBridge, nullptr);
    } catch (...) {
    }
    SubSystemModule::shutdown(context);
}

void enableGameFlowUIBridge(
    GameProject& project,
    GameFlowUIBridgeConfig config)
{
    auto configureRegistry = std::move(project.configureGameFlow);
    project.configureGameFlow = [
        configureRegistry = std::move(configureRegistry)](
            GameFlowActionRegistry& registry,
            std::string& error) mutable {
        if (configureRegistry && !configureRegistry(registry, error)) {
            return false;
        }
        return registerGameFlowUIActionTypes(registry, &error);
    };

    auto configureModules = std::move(project.configureModules);
    project.configureModules = [
        configureModules = std::move(configureModules),
        config = std::move(config)](
            EngineModuleRuntime& runtime) mutable {
        ayt::module::ModuleResult result =
            ayt::module::ModuleResult::success();
        if (configureModules) {
            result = configureModules(runtime);
            if (!result) return result;
        }
        return runtime.modules().emplace<GameFlowUIBridgeModule>(
            runtime.context().host(), std::move(config));
    };
}

GameFlowUIBridge* gameFlowUIBridge(IEngineHost& host) noexcept
{
    try {
        return host.service<GameFlowUIBridge>(kHostServiceGameFlowUIBridge);
    } catch (...) {
        return nullptr;
    }
}

} // namespace ayt::app

#include <AYApplication/UIFlowSceneBridgeModule.h>

#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/UIFlowRuntimeModule.h>
#include <AYApplication/UIFlowSceneComponents.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYEntity/EntityRuntimeModule.h>
#include <AYScene/SceneManager.h>

#include <memory>
#include <string>
#include <utility>

namespace ayt::app
{
namespace
{

class UIFlowSceneBridgeSubSystem final : public ayt::game::ISubSystem
{
public:
    UIFlowSceneBridgeSubSystem(
        IEngineHost& host,
        UIFlowRuntime& runtime,
        UIFlowSceneBridgeConfig config)
        : _host(host),
          _bridge(runtime, host.eventBus(), std::move(config)),
          _descriptor{
              .name = kUIFlowSceneBridgeSubSystemName.data(),
              .dependencies = {"Entity", "UIFlowRuntime"},
              .basePriority = 900,
              .timeType = ayt::game::SubSystemDescriptor::TimeType::Scaled,
              .phases = ayt::game::phaseBit(
                  ayt::game::FramePhase::World),
              .clock = ayt::game::ClockDomain::Game,
              .initializeAfter = {"Entity", "UIFlowRuntime"},
              .runsAfter = {"Entity"},
              .phasePriority = 900,
              .reads = {"Simulation.World"},
              .writes = {"Application.UIFlowCommands"},
          }
    {
        // The module dependency is optional so editor/preview hosts can use
        // this bridge without a game router. When a router is installed,
        // order initialization and World-phase polling after it so the first
        // scope key is the stable GameWorld id.
        if (gameWorldRouter(host) != nullptr) {
            _descriptor.dependencies.push_back("GameWorldRouter");
            _descriptor.initializeAfter.push_back("GameWorldRouter");
            _descriptor.runsAfter.push_back("GameWorldRouter");
        }
    }

    const char* getName() const override
    {
        return kUIFlowSceneBridgeSubSystemName.data();
    }

    const ayt::game::SubSystemDescriptor& getDescriptor() const override
    {
        return _descriptor;
    }

    bool initialize() override
    {
        ayt::scene::SceneManager* scenes = _host.scenes();
        if (scenes == nullptr) return false;
        return _bridge.start(scenes->current());
    }

    void update(float deltaTime) override { (void)deltaTime; }
    void fixedUpdate(float fixedDeltaTime) override
    {
        (void)fixedDeltaTime;
    }

    void tick(
        ayt::game::FramePhase phase,
        const ayt::game::FrameContext&) override
    {
        if (phase != ayt::game::FramePhase::World) return;
        ayt::scene::SceneManager* scenes = _host.scenes();
        if (scenes != nullptr) {
            (void)_bridge.update(scenes->current());
        }
    }

    void shutdown() override { _bridge.stop(); }

    UIFlowSceneBridge& bridge() noexcept { return _bridge; }

private:
    IEngineHost& _host;
    UIFlowSceneBridge _bridge;
    ayt::game::SubSystemDescriptor _descriptor;
};

UIFlowSceneBridgeConfig configureDefaultWorldResolver(
    IEngineHost& host,
    UIFlowSceneBridgeConfig config)
{
    if (!config.worldKeyResolver) {
        config.worldKeyResolver = [&host](const ayt::scene::Scene&) {
            IGameWorldRouter* router = gameWorldRouter(host);
            if (router == nullptr) return std::string{};
            const std::string_view pending = router->pendingWorldId();
            return std::string(
                pending.empty() ? router->currentWorldId() : pending);
        };
    }
    return config;
}

} // namespace

UIFlowSceneBridgeModule::UIFlowSceneBridgeModule(
    IEngineHost& host,
    UIFlowSceneBridgeConfig config)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kUIFlowSceneBridgeModuleId),
              .displayName = "AYApplication UI Flow Scene Bridge",
              .version = "0.3.0",
              .dependencies = {
                  ayt::module::ModuleDependency::required(
                      std::string(kUIFlowRuntimeModuleId)),
                  ayt::module::ModuleDependency::required(
                      std::string(ayt::entity::kEntityRuntimeModuleId)),
                  ayt::module::ModuleDependency::optional(
                      std::string(kGameWorldRouterModuleId)),
              }},
          std::string(kUIFlowSceneBridgeSubSystemName),
          [&host,
           config = configureDefaultWorldResolver(
               host, std::move(config))]() mutable
              -> std::unique_ptr<ayt::game::ISubSystem> {
              UIFlowRuntime* runtime = uiFlowRuntime(host);
              if (runtime == nullptr) return {};
              return std::make_unique<UIFlowSceneBridgeSubSystem>(
                  host, *runtime, std::move(config));
          },
          [&host](
              ayt::module::IModuleContext&,
              ayt::game::ISubSystem& system) {
              auto* typed = dynamic_cast<UIFlowSceneBridgeSubSystem*>(&system);
              if (typed == nullptr) {
                  return ayt::module::ModuleResult::failure(
                      ayt::module::ModuleErrorCode::InstallationFailed,
                      "UIFlowSceneBridge module received an incompatible "
                      "subsystem");
              }
              host.provide(kHostServiceUIFlowSceneBridge, &typed->bridge());
              return ayt::module::ModuleResult::success();
          }),
      _host(host)
{
}

ayt::module::ModuleResult UIFlowSceneBridgeModule::registerTypes(
    ayt::module::IModuleContext& context)
{
    auto* registry = context.findServiceAs<ayt::entity::ComponentRegistry>(
        ayt::entity::kComponentRegistryModuleService);
    if (registry == nullptr) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::TypeRegistrationFailed,
            "ComponentRegistry service is unavailable for the UI Flow "
            "Scene bridge");
    }
    const ayt::entity::ComponentRegistryResult result =
        registerUIFlowSceneComponents(*registry);
    if (!result) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::TypeRegistrationFailed,
            result.message());
    }
    return ayt::module::ModuleResult::success();
}

void UIFlowSceneBridgeModule::shutdown(
    ayt::module::IModuleContext& context) noexcept
{
    auto* installed =
        dynamic_cast<UIFlowSceneBridgeSubSystem*>(installedSubSystem());
    try {
        if (installed != nullptr
            && uiFlowSceneBridge(_host) == &installed->bridge()) {
            _host.provideService(kHostServiceUIFlowSceneBridge, nullptr);
        }
    } catch (...) {
    }
    SubSystemModule::shutdown(context);
}

UIFlowSceneBridge* uiFlowSceneBridge(IEngineHost& host) noexcept
{
    try {
        return host.service<UIFlowSceneBridge>(
            kHostServiceUIFlowSceneBridge);
    } catch (...) {
        return nullptr;
    }
}

} // namespace ayt::app

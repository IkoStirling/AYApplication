#include <AYOnlineApplication/OnlineApplicationRuntimeModule.h>

#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/RuntimeSceneLoaderModule.h>
#include <AYNetwork/NetworkRuntimeModule.h>
#include <AYNetwork/Session/OnlineFlowRuntimeModule.h>
#include <AYNetwork/Session/OnlineRuntimeModule.h>

#include <string>
#include <utility>

namespace ayt::app::online
{
namespace
{

ayt::net::IOnlineFlowSubSystem* findOnlineFlow(
    ayt::module::IModuleContext& context) noexcept
{
    auto* service = context.findServiceAs<ayt::game::ISubSystemModuleService>(
        ayt::game::kSubSystemModuleService);
    if (service == nullptr) {
        return nullptr;
    }
    return dynamic_cast<ayt::net::IOnlineFlowSubSystem*>(
        service->findSubSystem("OnlineFlow"));
}

ayt::module::ModuleResult invalidConfiguration(std::string message)
{
    return ayt::module::ModuleResult::failure(
        ayt::module::ModuleErrorCode::InvalidDescriptor,
        std::move(message));
}

} // namespace

OnlineApplicationRuntimeModule::OnlineApplicationRuntimeModule(
    ayt::app::IEngineHost& host,
    OnlineSceneBridgeConfig config)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kOnlineApplicationRuntimeModuleId),
              .displayName = "AYOnlineApplication Runtime",
              .version = "1.0.0",
              .dependencies = {
                  ayt::module::ModuleDependency::required(
                      std::string(ayt::net::kOnlineFlowRuntimeModuleId)),
                  ayt::module::ModuleDependency::required(
                      std::string(ayt::app::kRuntimeSceneLoaderModuleId))}},
          "OnlineApplication",
          [&host, config = std::move(config)](
              ayt::module::IModuleContext& context) {
              auto* flow = findOnlineFlow(context);
              if (flow == nullptr || !config.isValid()) {
                  return std::unique_ptr<ayt::game::ISubSystem>{};
              }
              return std::unique_ptr<ayt::game::ISubSystem>(
                  createOnlineApplicationSubSystem(
                      host, *flow, config));
          })
{
}

ayt::module::ModuleResult configureOnlineApplicationModules(
    ayt::app::EngineModuleRuntime& runtime,
    OnlineApplicationConfig config,
    ayt::net::OnlineSubSystemDependencies dependencies)
{
    auto& modules = runtime.modules();
    if (modules.phase() != ayt::module::ModuleManagerPhase::Collecting) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::InvalidPhase,
            "Online modules must be configured before module resolution");
    }
    if (modules.find(ayt::app::kRuntimeSceneLoaderModuleId) == nullptr) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::MissingDependency,
            "OnlineApplication requires the Client RuntimeSceneLoader module");
    }

    const bool hasInjectedServices = dependencies.hasAnyBackendService();
    if (!config.isValid()) {
        return invalidConfiguration("OnlineApplicationConfig is invalid");
    }
    if (hasInjectedServices != dependencies.hasCompleteBackendServices()) {
        return invalidConfiguration(
            "Online backend dependencies must be empty or complete");
    }
    if (!hasInjectedServices && !config.online.isValidForHttp()) {
        return invalidConfiguration(
            "Online HTTP backend configuration is invalid");
    }

    if (modules.find(ayt::net::kNetworkRuntimeModuleId) == nullptr) {
        if (auto result = modules.emplace<ayt::net::NetworkRuntimeModule>();
            !result) {
            return result;
        }
    }
    if (modules.find(ayt::net::kOnlineRuntimeModuleId) == nullptr) {
        if (auto result = modules.emplace<ayt::net::OnlineRuntimeModule>(
                config.online,
                dependencies,
                &runtime.context().host().eventBus()); !result) {
            return result;
        }
    }
    if (modules.find(ayt::net::kOnlineFlowRuntimeModuleId) == nullptr) {
        if (auto result = modules.emplace<ayt::net::OnlineFlowRuntimeModule>(
                config.flow,
                &runtime.context().host().eventBus()); !result) {
            return result;
        }
    }
    if (modules.find(kOnlineApplicationRuntimeModuleId) == nullptr) {
        if (auto result = modules.emplace<OnlineApplicationRuntimeModule>(
                runtime.context().host(),
                std::move(config.scenes)); !result) {
            return result;
        }
    }

    return ayt::module::ModuleResult::success();
}

} // namespace ayt::app::online

#include <AYApplication/GameFlowRuntimeModule.h>

#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>

#include <memory>
#include <utility>

namespace ayt::app
{
namespace
{

ayt::module::ModuleDescriptor moduleDescriptor(bool worldActions)
{
    ayt::module::ModuleDescriptor descriptor{
        .id = std::string(kGameFlowRuntimeModuleId),
        .displayName = "AYApplication GameFlow Runtime",
        .version = "1.0.0",
    };
    if (worldActions) {
        descriptor.dependencies = {
            ayt::module::ModuleDependency::required(
                std::string(kGameWorldRouterModuleId))};
    }
    return descriptor;
}

struct GameFlowRuntimeSeed
{
    IEngineHost* host = nullptr;
    GameFlowRuntimeConfig config;
    std::unique_ptr<GameFlowRuntimePreparation> preparation;
};

} // namespace

GameFlowRuntimeModule::GameFlowRuntimeModule(
    IEngineHost& host,
    GameFlowRuntimeConfig config)
    : SubSystemModule(
          moduleDescriptor(config.enableWorldActions),
          std::string(kGameFlowRuntimeSubSystemName),
          [seed = std::make_shared<GameFlowRuntimeSeed>(
               GameFlowRuntimeSeed{&host, std::move(config), {}})]() mutable {
              return std::make_unique<GameFlowRuntime>(
                  *seed->host, std::move(seed->config));
          },
          [&host](ayt::module::IModuleContext&,
                  ayt::game::ISubSystem& system) {
              auto* runtime = dynamic_cast<GameFlowRuntime*>(&system);
              if (runtime == nullptr) {
                  return ayt::module::ModuleResult::failure(
                      ayt::module::ModuleErrorCode::InstallationFailed,
                      "GameFlowRuntime module received an incompatible subsystem");
              }
              host.provide(kHostServiceGameFlowRuntime, runtime);
              return ayt::module::ModuleResult::success();
          }),
      _host(host)
{
}

GameFlowRuntimeModule::GameFlowRuntimeModule(
    IEngineHost& host,
    std::unique_ptr<GameFlowRuntimePreparation> preparation)
    : SubSystemModule(
          moduleDescriptor(
              preparation && preparation->worldActionsEnabled()),
          std::string(kGameFlowRuntimeSubSystemName),
          [seed = std::make_shared<GameFlowRuntimeSeed>(
               GameFlowRuntimeSeed{&host, {}, std::move(preparation)})]() mutable {
              return std::make_unique<GameFlowRuntime>(
                  *seed->host, std::move(seed->preparation));
          },
          [&host](ayt::module::IModuleContext&,
                  ayt::game::ISubSystem& system) {
              auto* runtime = dynamic_cast<GameFlowRuntime*>(&system);
              if (runtime == nullptr) {
                  return ayt::module::ModuleResult::failure(
                      ayt::module::ModuleErrorCode::InstallationFailed,
                      "GameFlowRuntime module received an incompatible subsystem");
              }
              host.provide(kHostServiceGameFlowRuntime, runtime);
              return ayt::module::ModuleResult::success();
          }),
      _host(host)
{
}

void GameFlowRuntimeModule::shutdown(
    ayt::module::IModuleContext& context) noexcept
{
    // The GameLoop may already have destroyed its subsystem registry. Pointer
    // value comparison is safe here; RTTI on that stale pointer is not.
    void* installed = static_cast<void*>(installedSubSystem());
    try {
        if (installed != nullptr
            && _host.findService(kHostServiceGameFlowRuntime) == installed) {
            _host.provideService(kHostServiceGameFlowRuntime, nullptr);
        }
    } catch (...) {
    }
    SubSystemModule::shutdown(context);
}

} // namespace ayt::app

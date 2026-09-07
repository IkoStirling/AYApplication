#include <AYApplication/RuntimeSceneLoaderModule.h>

#include <AYEntity/EntityRuntimeModule.h>

#include <string>
#include <utility>

namespace ayt::app
{

RuntimeSceneLoaderModule::RuntimeSceneLoaderModule(
    ayt::scene::SceneManager& scenes,
    RuntimeSceneLoaderConfig config,
    ayt::event::EventBus* eventBus)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kRuntimeSceneLoaderModuleId),
              .displayName = "AYApplication Runtime Scene Loader",
              .version = "0.1.0",
              .dependencies = {
                  ayt::module::ModuleDependency::required(
                      std::string(ayt::entity::kEntityRuntimeModuleId))}},
          "RuntimeSceneLoader",
          [&scenes, config = std::move(config), eventBus]() {
              return createRuntimeSceneLoader(
                  scenes, config, eventBus);
          })
{
}

} // namespace ayt::app

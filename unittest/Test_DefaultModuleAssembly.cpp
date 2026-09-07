#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RegisterDefaultModules.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYGameLoop/SubSystemRegistry.h>
#include <AYModule/ModuleTypes.h>
#include <AYTest.h>

#include <string>
#include <vector>

namespace ayt::app::test
{

TEST_SUITE(DefaultModuleAssemblyTests)

TEST_CASE(client_graph_installs_in_dependency_order_and_cleans_up)
{
    auto& registry = ayt::game::SubSystemRegistry::instance();
    registry.clearAll();

    EngineModuleRuntime runtime(defaultEngineHost());
    ClientModuleOptions options{};
    options.enableAudio = false;
    options.enablePresentation = false;
    options.enablePhysics = true;

    CHECK_TRUE(configureDefaultClientModules(runtime, options).succeeded());
    CHECK(runtime.modules().size() == 8);
    CHECK_TRUE(runtime.prepare().succeeded());
    runtime.context().componentRegistry().seal();

    const std::vector<ayt::module::ModuleId> expectedOrder{
        "AYDevice.Runtime",
        "AYEntity.Components",
        "AYEntity.Runtime",
        "AYPhysics.Runtime",
        "AYEntity.PhysicsIntegration",
        "AYEntity.ScriptIntegration",
        "AYScript.Runtime",
        "AYApplication.RuntimeSceneLoader"};
    CHECK(runtime.modules().orderedModuleIds() == expectedOrder);

    CHECK_TRUE(runtime.install().succeeded());
    CHECK_NOT_NULL(registry.findSubSystem("Device"));
    CHECK_NOT_NULL(registry.findSubSystem("Entity"));
    CHECK_NOT_NULL(registry.findSubSystem("Physics"));
    CHECK_NOT_NULL(registry.findSubSystem("EntityPhysicsBridge"));
    CHECK_NOT_NULL(registry.findSubSystem("ayt.script.runtime"));
    CHECK_NOT_NULL(registry.findSubSystem("RuntimeSceneLoader"));
    CHECK(registry.findSubSystem("Renderer") == nullptr);
    CHECK(registry.findSubSystem("Audio") == nullptr);

    bindBuiltinHostServices(defaultEngineHost());
    CHECK(defaultEngineHost().service<IRuntimeSceneLoader>(
              kHostServiceRuntimeSceneLoader)
          == findRegisteredRuntimeSceneLoader());

    runtime.shutdown();
    defaultEngineHost().provide<IRuntimeSceneLoader>(
        kHostServiceRuntimeSceneLoader, nullptr);
    CHECK(registry.getCount() == 0);
}

TEST_CASE(server_graph_can_prepare_after_the_component_registry_is_sealed)
{
    auto& registry = ayt::game::SubSystemRegistry::instance();
    CHECK(registry.getCount() == 0);
    CHECK_TRUE(ayt::entity::ComponentRegistry::instance().isSealed());

    EngineModuleRuntime runtime(defaultEngineHost());
    ServerModuleOptions options{};
    options.enablePhysics = false;
    options.enableScript = true;

    CHECK_TRUE(configureDefaultServerModules(runtime, options).succeeded());
    CHECK_TRUE(runtime.prepare().succeeded());
    CHECK_TRUE(runtime.install().succeeded());
    CHECK_NOT_NULL(registry.findSubSystem("Entity"));
    CHECK_NOT_NULL(registry.findSubSystem("ayt.script.runtime"));
    CHECK(registry.findSubSystem("Device") == nullptr);
    CHECK(registry.findSubSystem("Physics") == nullptr);
    CHECK(registry.findSubSystem("EntityPhysicsBridge") == nullptr);

    runtime.shutdown();
    CHECK(registry.getCount() == 0);
}

TEST_SUITE_END

} // namespace ayt::app::test

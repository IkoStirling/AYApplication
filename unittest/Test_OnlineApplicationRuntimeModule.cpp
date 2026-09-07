#include <AYOnlineApplication/OnlineApplicationRuntimeModule.h>

#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/RuntimeSceneLoaderModule.h>
#include <AYGameLoop/IGameLoop.h>
#include <AYModule/IModule.h>
#include <AYNetwork/NetworkModule.h>
#include <AYNetwork/NetworkRuntimeModule.h>
#include <AYNetwork/Session/OnlineFlowRuntimeModule.h>
#include <AYNetwork/Session/OnlineFlowSubSystem.h>
#include <AYNetwork/Session/OnlineRuntimeModule.h>
#include <AYNetwork/Session/OnlineSubSystem.h>
#include <AYTest.h>

#include <memory>
#include <string>
#include <utility>

namespace ayt::app::online::test
{
namespace
{

class RuntimeSceneLoaderMarkerModule final : public ayt::module::IModule
{
public:
    RuntimeSceneLoaderMarkerModule()
        : _descriptor{
              .id = std::string(ayt::app::kRuntimeSceneLoaderModuleId),
              .displayName = "RuntimeSceneLoader Test Marker",
              .version = "1.0.0",
              .dependencies = {}}
    {
    }

    const ayt::module::ModuleDescriptor& descriptor()
        const noexcept override
    {
        return _descriptor;
    }

private:
    ayt::module::ModuleDescriptor _descriptor;
};

OnlineApplicationConfig makeModuleConfig()
{
    OnlineApplicationConfig config;
    config.online.localPeerId = ayt::net::PeerId{"module-client"};
    config.online.backend.serverPort = 443;
    config.online.backend.useTls = true;
    config.online.p2p.p2p.localPeerId = config.online.localPeerId;
    config.online.sessions.localPeerId = config.online.localPeerId;
    config.scenes.contentResolver =
        std::make_shared<OnlineContentCatalog>();
    config.scenes.mainMenuScenePath = "Content/Scenes/MainMenu.ayscene";
    return config;
}

void clearOnlineSubSystems()
{
    auto& loop = ayt::game::IGameLoop::instance();
    loop.unregisterSubSystem("OnlineApplication");
    loop.unregisterSubSystem("OnlineFlow");
    loop.unregisterSubSystem("Online");
    loop.unregisterSubSystem("Network");
}

} // namespace

TEST_SUITE(OnlineApplicationRuntimeModuleTests)

TEST_CASE(configures_and_installs_the_optional_client_stack)
{
    clearOnlineSubSystems();
    ayt::app::EngineModuleRuntime runtime(
        ayt::app::defaultEngineHost());
    CHECK_TRUE(runtime.modules().emplace<
        RuntimeSceneLoaderMarkerModule>().succeeded());

    CHECK_TRUE(configureOnlineApplicationModules(
        runtime, makeModuleConfig()).succeeded());
    CHECK(runtime.modules().size() == 5);
    CHECK_NOT_NULL(runtime.modules().find(
        ayt::net::kNetworkRuntimeModuleId));
    CHECK_NOT_NULL(runtime.modules().find(
        ayt::net::kOnlineRuntimeModuleId));
    CHECK_NOT_NULL(runtime.modules().find(
        ayt::net::kOnlineFlowRuntimeModuleId));
    CHECK_NOT_NULL(runtime.modules().find(
        kOnlineApplicationRuntimeModuleId));

    CHECK_TRUE(runtime.prepare().succeeded());
    CHECK_TRUE(runtime.install().succeeded());
    CHECK_NOT_NULL(ayt::net::findRegisteredNetworkSubSystem());
    CHECK_NOT_NULL(ayt::net::findRegisteredOnlineSubSystem());
    CHECK_NOT_NULL(ayt::net::findRegisteredOnlineFlowSubSystem());
    CHECK_NOT_NULL(findRegisteredOnlineApplicationSubSystem());

    runtime.shutdown();
    CHECK(ayt::net::findRegisteredNetworkSubSystem() == nullptr);
    CHECK(ayt::net::findRegisteredOnlineSubSystem() == nullptr);
    CHECK(ayt::net::findRegisteredOnlineFlowSubSystem() == nullptr);
    CHECK(findRegisteredOnlineApplicationSubSystem() == nullptr);
}

TEST_CASE(requires_default_client_scene_loader_before_mutation)
{
    ayt::app::EngineModuleRuntime runtime(
        ayt::app::defaultEngineHost());
    const auto result = configureOnlineApplicationModules(
        runtime, makeModuleConfig());
    CHECK_FALSE(result.succeeded());
    CHECK(result.code() == ayt::module::ModuleErrorCode::MissingDependency);
    CHECK(runtime.modules().size() == 0);
}

TEST_SUITE_END

} // namespace ayt::app::online::test

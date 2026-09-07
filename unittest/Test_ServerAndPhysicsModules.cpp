// Test_ServerAndPhysicsModules.cpp — Server assembly + PhysicsSubSystem (E-1)
//
// Drives registerDefaultServerModules / ApplicationImpl -server without entering
// GameLoop::run(). Initializes PhysicsSubSystem manually to verify Host
// physics() lazy fallback and fixedUpdate stepping.

#include <AYApplication.h>
#include <AYGameLoop.h>
#include <AYPhysics/PhysicsSubSystem.h>
#include <AYPhysics/PhysicsTypes.h>
#include <AYApplication/RegisterDefaultModules.h>
#include <AYApplication/IEngineHost.h>

#include <AYTest.h>

using namespace ayt::app;

namespace
{

void clearCompatibilityServerSubSystems()
{
    providePhysicsQuery(defaultEngineHost(), nullptr);
    providePhysics(defaultEngineHost(), nullptr);

    auto& loop = ayt::game::IGameLoop::instance();
    loop.unregisterSubSystem("Script");
    loop.unregisterSubSystem("EntityPhysicsBridge");
    loop.unregisterSubSystem("Physics");
    loop.unregisterSubSystem("Entity");
}

} // namespace

TEST_SUITE(ServerAndPhysicsModules)

TEST_CASE(server_modules_register_physics_and_host_lazy)
{
    clearCompatibilityServerSubSystems();

    ServerModuleOptions opts;
    opts.enableScript = false;
    opts.enablePhysics = true;
    registerDefaultServerModules(opts);

    auto* phys = ayt::physics::PhysicsSubSystem::findRegistered();
    CHECK(phys != nullptr);

    EngineHostScope scope(defaultEngineHost());
    bindBuiltinHostServices(defaultEngineHost());

    CHECK(phys->initialize());
    CHECK(phys->manager() != nullptr);
    CHECK(defaultEngineHost().physics() == phys->manager());

    phys->shutdown();
    clearCompatibilityServerSubSystems();
}

TEST_CASE(application_server_mode_wires_physics)
{
    clearCompatibilityServerSubSystems();

    GameDesc desc;
    desc.serverMode = true;
    desc.enablePhysics = true;
    bool projectModulesConfigured = false;
    desc.configureModules = [&](EngineModuleRuntime& runtime) {
        projectModulesConfigured = true;
        CHECK(runtime.modules().phase() ==
              ayt::module::ModuleManagerPhase::Collecting);
        return ayt::module::ModuleResult::success();
    };
    AppCommandLine cmd;
    cmd.server = true;
    auto app = IApplication::create(desc, cmd);
    CHECK(app != nullptr);

    app->registerSubSystems();
    CHECK(projectModulesConfigured);

    auto* phys = ayt::physics::PhysicsSubSystem::findRegistered();
    CHECK(phys != nullptr);
    CHECK(phys->initialize());

    EngineHostScope scope(defaultEngineHost());
    CHECK(defaultEngineHost().physics() == phys->manager());
    phys->shutdown();

    app.reset();
    clearCompatibilityServerSubSystems();
}

TEST_CASE(physics_fixed_update_steps_mock_backend)
{
    clearCompatibilityServerSubSystems();

    ayt::physics::PhysicsBackendDescriptor desc{};
    desc.kind3D = ayt::physics::BackendKind::Mock;
    registerPhysicsModule(desc);

    auto* phys = ayt::physics::PhysicsSubSystem::findRegistered();
    CHECK(phys != nullptr);
    CHECK(phys->initialize());
    CHECK(phys->manager() != nullptr);

    phys->fixedUpdate(1.0f / 60.0f);
    phys->update(1.0f / 60.0f);

    phys->shutdown();
    clearCompatibilityServerSubSystems();
}

TEST_SUITE_END

// Test_ServerAndPhysicsModules.cpp — Server assembly + PhysicsSubSystem (E-1)
//
// Drives registerDefaultServerModules / ApplicationImpl -server without entering
// GameLoop::run(). Initializes PhysicsSubSystem manually to verify Host
// physics() lazy fallback and fixedUpdate stepping.

#include <AYApplication.h>
#include <AYGameLoop.h>
#include <AYPhysicsSubSystem.h>
#include <AYPhysicsTypes.h>
#include <AYRegisterDefaultModules.h>
#include <IEngineHost.h>

#include <AYTest.h>

using namespace ayt::app;

TEST_SUITE(ServerAndPhysicsModules)

TEST_CASE(server_modules_register_physics_and_host_lazy)
{
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
}

TEST_CASE(application_server_mode_wires_physics)
{
    GameDesc desc;
    desc.serverMode = true;
    desc.enablePhysics = true;
    AppCommandLine cmd;
    cmd.server = true;
    auto app = IApplication::create(desc, cmd);
    CHECK(app != nullptr);

    app->registerSubSystems();

    auto* phys = ayt::physics::PhysicsSubSystem::findRegistered();
    CHECK(phys != nullptr);
    CHECK(phys->initialize());

    EngineHostScope scope(defaultEngineHost());
    CHECK(defaultEngineHost().physics() == phys->manager());
    phys->shutdown();
}

TEST_CASE(physics_fixed_update_steps_mock_backend)
{
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
}

TEST_SUITE_END

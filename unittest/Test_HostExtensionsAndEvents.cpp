// Test_HostExtensionsAndEvents.cpp — Host §6 + SimTick / Scene lifecycle (P4/P5)

#include <AYApplication.h>
#include <AYGameLoop.h>
#include <AYGameLoop/GameLoopEvents.h>
#include <AYPhysics/PhysicsSubSystem.h>
#include <AYPhysics/PhysicsTypes.h>
#include <AYApplication/RegisterDefaultModules.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
#include <AYApplication/DeprecatedSuppress.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/IHostPluginHooks.h>
#include <AYPhysics/IPhysicsQuery.h>

#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>

#include <AYTest.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ayt::app;

namespace {

struct CountingPlugin final : public IHostPluginHooks {
    const char* name() const override { return "CountingPlugin"; }
    void onAttach(IEngineHost& host) override
    {
        ++attachCount;
        bus = &host.eventBus();
        conn = bus->subscribe<ayt::game::SimTickEvent>(
            [this](const ayt::game::SimTickEvent& e) {
                lastSimTick = e.simTick;
                ++simTicksSeen;
            });
    }
    void onDetach(IEngineHost& host) override
    {
        (void)host;
        if (bus && conn != 0) {
            bus->unsubscribe(conn);
            conn = 0;
        }
        ++detachCount;
    }

    ayt::event::EventBus* bus = nullptr;
    ayt::event::ConnectionId conn = 0;
    int attachCount = 0;
    int detachCount = 0;
    std::atomic<int> simTicksSeen{0};
    uint64_t lastSimTick = 0;
};

struct InjectedQuery final : public ayt::physics::IPhysicsQuery {
    bool ready = true;
    bool isReady() const override { return ready; }
    ayt::physics::PhysResult raycastSync(const ayt::math::Ray&,
                                         ayt::physics::RaycastHit&,
                                         ayt::physics::PhysLayerMask) override
    {
        ++raycasts;
        return ayt::physics::PhysResult::Ok;
    }
    ayt::physics::PhysResult overlapSphereSync(const ayt::math::FVector3&,
                                               float,
                                               std::vector<ayt::physics::BodyHandle>&,
                                               ayt::physics::PhysLayerMask) override
    {
        return ayt::physics::PhysResult::Unsupported;
    }
    int raycasts = 0;
};

class IsolatingHost final : public IEngineHost {
public:
    ayt::game::IGameLoop& gameLoop() override { return ayt::game::GameLoop::instance(); }
    ayt::event::EventBus& eventBus() override { return _bus; }
    ayt::game::ISubSystem* findSubSystem(const char* name) override
    {
        return ayt::game::SubSystemRegistry::instance().findSubSystem(name);
    }
    void provideService(std::string_view key, void* instance) override
    {
        if (key.empty()) return;
        if (!instance) _map.erase(std::string(key));
        else _map[std::string(key)] = instance;
    }
    void* findService(std::string_view key) const override
    {
        const auto it = _map.find(std::string(key));
        return it == _map.end() ? nullptr : it->second;
    }
    void clearProvidedServices() override { _map.clear(); }
    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override
    {
        return static_cast<ayt::physics::PhysicsManager*>(findService(kHostServicePhysics));
    }
    ayt::physics::IPhysicsQuery* physicsQuery() override
    {
        return static_cast<ayt::physics::IPhysicsQuery*>(findService(kHostServicePhysicsQuery));
    }
    ayt::audio::AudioEngine* audio() override { return nullptr; }
    ayt::scene::SceneManager* scenes() override
    {
        return static_cast<ayt::scene::SceneManager*>(findService(kHostServiceScenes));
    }

private:
    ayt::event::EventBus _bus;
    std::unordered_map<std::string, void*> _map;
};

} // namespace

TEST_SUITE(HostExtensionsAndEvents)

TEST_CASE(multi_host_injects_physics_query_and_event_bus)
{
    IsolatingHost host;
    InjectedQuery query;
    EngineHostScope scope(host);

    CHECK(currentEngineHost() == &host);
    CHECK(&resolveEventBus() == &host.eventBus());

    providePhysicsQuery(host, &query);
    CHECK(host.physicsQuery() == &query);

    ayt::physics::RaycastHit hit{};
    ayt::math::Ray ray{};
    CHECK(host.physicsQuery()->raycastSync(ray, hit) == ayt::physics::PhysResult::Ok);
    CHECK(query.raycasts == 1);
}

TEST_CASE(default_host_plugin_attach_sees_sim_tick)
{
    CountingPlugin plugin;
    defaultEngineHost().registerPlugin(&plugin);
    CHECK(plugin.attachCount == 1);

    auto& loop = ayt::game::GameLoop::instance();
    const uint64_t before = loop.getSimTick();
    CHECK(loop.preparePlaySession());
    loop.stepOnce();
    loop.endPlaySession();

    CHECK(loop.getSimTick() > before);
    CHECK(plugin.simTicksSeen.load() >= 1);

    defaultEngineHost().unregisterPlugin(&plugin);
    CHECK(plugin.detachCount == 1);
}

TEST_CASE(scene_lifecycle_emits_begin_and_end_play)
{
    EngineHostScope scope(defaultEngineHost());
    bindBuiltinHostServices(defaultEngineHost());

    int beginCount = 0;
    int endCount = 0;
    ayt::scene::Scene* seenPlay = nullptr;

    auto& bus = resolveEventBus();
    auto c1 = bus.subscribe<ayt::event::SceneBeginPlayEvent>(
        [&](const ayt::event::SceneBeginPlayEvent& e) {
            ++beginCount;
            seenPlay = e.play;
        });
    auto c2 = bus.subscribe<ayt::event::SceneEndPlayEvent>(
        [&](const ayt::event::SceneEndPlayEvent&) { ++endCount; });

    AY_DEPRECATED_SUPPRESS_BEGIN
    auto& sm = ayt::scene::SceneManager::instance();
    AY_DEPRECATED_SUPPRESS_END
    sm.endPlay();
    sm.setEdit(nullptr);
    sm.setCurrent(nullptr);

    auto edit = std::make_unique<ayt::scene::Scene>(ayt::scene::SceneMode::Edit, "host-evt");
    sm.setEdit(edit.get());
    CHECK(sm.beginPlay());
    CHECK(beginCount == 1);
    CHECK(seenPlay == sm.play());

    sm.endPlay();
    CHECK(endCount == 1);

    bus.unsubscribe(c1);
    bus.unsubscribe(c2);
    sm.setEdit(nullptr);
}

TEST_CASE(physics_query_lazy_after_subsystem_init)
{
    ayt::physics::PhysicsBackendDescriptor desc{};
    desc.kind3D = ayt::physics::BackendKind::Mock;
    registerPhysicsModule(desc);

    auto* phys = ayt::physics::PhysicsSubSystem::findRegistered();
    CHECK(phys != nullptr);
    CHECK(phys->initialize());

    EngineHostScope scope(defaultEngineHost());
    CHECK(defaultEngineHost().physicsQuery() != nullptr);
    CHECK(defaultEngineHost().physicsQuery()->isReady());

    phys->shutdown();
}

TEST_SUITE_END

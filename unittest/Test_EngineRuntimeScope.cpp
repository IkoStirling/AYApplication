#include <AYApplication/DeprecatedSuppress.h>
#include <AYApplication/EngineRuntimeScope.h>
#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYEntity/World.h>
#include <AYEventSystem/EventBus.h>
#include <AYGameLoop.h>
#include <AYScene.h>
#include <AYScene/SceneLifecycleObserver.h>
#include <AYScene/SceneManager.h>
#include <AYTask/TaskCompletionHook.h>
#include <AYTest.h>

#include <string>
#include <unordered_map>

namespace
{

class ScopeHost final : public ayt::app::IEngineHost
{
public:
    ayt::game::IGameLoop& gameLoop() override
    {
        return ayt::game::GameLoop::instance();
    }
    ayt::event::EventBus& eventBus() override { return _events; }
    ayt::game::ISubSystem* findSubSystem(const char*) override { return nullptr; }
    void provideService(std::string_view key, void* value) override
    {
        if (value == nullptr) {
            services.erase(std::string(key));
        } else {
            services[std::string(key)] = value;
        }
    }
    void* findService(std::string_view key) const override
    {
        const auto it = services.find(std::string(key));
        return it == services.end() ? nullptr : it->second;
    }
    void clearProvidedServices() override { services.clear(); }
    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override { return nullptr; }
    ayt::physics::IPhysicsQuery* physicsQuery() override { return nullptr; }
    ayt::audio::AudioEngine* audio() override { return nullptr; }
    ayt::scene::SceneManager* scenes() override
    {
        return static_cast<ayt::scene::SceneManager*>(
            findService(ayt::app::kHostServiceScenes));
    }

    std::unordered_map<std::string, void*> services;

private:
    ayt::event::EventBus _events;
};

class Observer final : public ayt::scene::ISceneLifecycleObserver
{
public:
    void onCurrentChanged(ayt::scene::Scene*) override { ++currentChanged; }
    void onBeginPlay(ayt::scene::Scene*) override {}
    void onEndPlay(ayt::scene::Scene*) override {}

    int currentChanged = 0;
};

void savedTaskHook(const ayt::task::TaskCompletionNotice&) {}

} // namespace

TEST_SUITE(EngineRuntimeScopeTests)

TEST_CASE(restores_services_hooks_scene_and_active_world_as_one_scope)
{
    using namespace ayt::app;

    ScopeHost host;
    int sentinels[8]{};
    const char* keys[] = {
        kHostServiceResources,
        kHostServicePhysics,
        kHostServicePhysicsQuery,
        kHostServiceAudio,
        kHostServiceDeviceManager,
        kHostServiceScenes,
        kHostServiceRuntimeSceneLoader,
        kHostServiceGameWorldRouter,
    };
    for (std::size_t i = 0; i < 8; ++i) {
        host.provideService(keys[i], &sentinels[i]);
    }

    AY_DEPRECATED_SUPPRESS_BEGIN
    auto& scenes = ayt::scene::SceneManager::instance();
    AY_DEPRECATED_SUPPRESS_END
    scenes.endPlay();
    ayt::scene::Scene saved(ayt::scene::SceneMode::Edit, "saved");
    ayt::scene::Scene runtime(ayt::scene::SceneMode::Edit, "runtime");
    Observer observer;
    scenes.setLifecycleObserver(&observer);
    scenes.setEdit(&saved);
    scenes.setCurrent(&saved);
    const int observerCallsBeforeScope = observer.currentChanged;
    ayt::task::setTaskCompletionHook(&savedTaskHook);

    {
        EngineRuntimeScope scope(host);
        CHECK(scope.active());
        CHECK(host.findService(kHostServiceResources) != &sentinels[0]);
        CHECK(host.findService(kHostServiceScenes) == &scenes);
        CHECK(scenes.lifecycleObserver() != &observer);
        CHECK(ayt::task::getTaskCompletionHook() != &savedTaskHook);

        // Module installation happens after the scope captures its snapshot.
        // A service published by that runtime must not survive scope teardown.
        int runtimePhysics = 0;
        host.provideService(kHostServicePhysics, &runtimePhysics);
        scope.refresh();
        CHECK(host.findService(kHostServicePhysics) == &runtimePhysics);

        scenes.setEdit(&runtime);
        scenes.setCurrent(&runtime);
        CHECK(ayt::entity::World::activeWorld() == &runtime.world());
    }

    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(host.findService(keys[i]) == &sentinels[i]);
    }
    CHECK(scenes.lifecycleObserver() == &observer);
    CHECK(scenes.edit() == &saved);
    CHECK(scenes.current() == &saved);
    CHECK(ayt::entity::World::activeWorld() == &saved.world());
    CHECK(ayt::task::getTaskCompletionHook() == &savedTaskHook);
    CHECK(observer.currentChanged == observerCallsBeforeScope);

    scenes.setLifecycleObserver(nullptr);
    scenes.setEdit(nullptr);
    scenes.setCurrent(nullptr);
    ayt::task::setTaskCompletionHook(nullptr);
}

TEST_SUITE_END

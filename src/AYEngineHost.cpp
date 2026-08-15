#include <AYApplication/IEngineHost.h>

#include <AYAudio/AudioSubSystem.h>
#include <AYGameLoop.h>
#include <AYPhysics/PhysicsSubSystem.h>
#include <AYResource/ResourceManager.h>
#include <AYApplication/SceneLifecycleEventBridge.h>
#include <AYScene/SceneManager.h>  // PR-6 (v0.1.3): scenes() facade
#include <AYGameLoop/SubSystemRegistry.h>
#include <AYApplication/DeprecatedSuppress.h>  // v0.3 PR-4 (AYScene): instance() [[deprecated]] 豁免
#include <AYPhysics/IPhysicsQuery.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/TaskEvents.h>
#include <AYTask/TaskCompletionHook.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::app
{

namespace {

class DefaultEngineHost final : public IEngineHost {
public:
    ayt::game::IGameLoop& gameLoop() override
    {
        return ayt::game::GameLoop::instance();
    }

    ayt::event::EventBus& eventBus() override
    {
        return ayt::event::EventBus::instance();
    }

    ayt::game::ISubSystem* findSubSystem(const char* name) override
    {
        if (name == nullptr || name[0] == '\0') {
            return nullptr;
        }
        return ayt::game::SubSystemRegistry::instance().findSubSystem(name);
    }

    void provideService(std::string_view key, void* instance) override
    {
        if (key.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        if (instance == nullptr) {
            _services.erase(std::string(key));
        } else {
            _services[std::string(key)] = instance;
        }
    }

    void* findService(std::string_view key) const override
    {
        if (key.empty()) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _services.find(std::string(key));
        return it == _services.end() ? nullptr : it->second;
    }

    void clearProvidedServices() override
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _services.clear();
    }

    ayt::resource::ResourceManager* resources() override
    {
        if (auto* p = static_cast<ayt::resource::ResourceManager*>(
                findService(kHostServiceResources))) {
            return p;
        }
        return &ayt::resource::ResourceManager::instance();
    }

    ayt::physics::PhysicsManager* physics() override
    {
        if (auto* p = static_cast<ayt::physics::PhysicsManager*>(
                findService(kHostServicePhysics))) {
            return p;
        }
        if (auto* physSub = ayt::physics::PhysicsSubSystem::findRegistered()) {
            return physSub->manager();
        }
        return nullptr;
    }

    ayt::physics::IPhysicsQuery* physicsQuery() override
    {
        if (auto* p = static_cast<ayt::physics::IPhysicsQuery*>(
                findService(kHostServicePhysicsQuery))) {
            return p;
        }
        if (auto* physSub = ayt::physics::PhysicsSubSystem::findRegistered()) {
            return physSub->query();
        }
        return nullptr;
    }

    ayt::audio::AudioEngine* audio() override
    {
        if (auto* p = static_cast<ayt::audio::AudioEngine*>(
                findService(kHostServiceAudio))) {
            return p;
        }
        auto* sub = findSubSystem("Audio");
        if (auto* audioSub = dynamic_cast<ayt::audio::AudioSubSystem*>(sub)) {
            return audioSub->engine();
        }
        return nullptr;
    }

    AY_DEPRECATED_SUPPRESS_BEGIN
    ayt::scene::SceneManager* scenes() override
    {
        if (auto* p = static_cast<ayt::scene::SceneManager*>(
                findService(kHostServiceScenes))) {
            return p;
        }
        return &ayt::scene::SceneManager::instance();
    }
    AY_DEPRECATED_SUPPRESS_END

    void registerPlugin(IHostPluginHooks* plugin) override
    {
        if (plugin == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(_pluginMutex);
        if (std::find(_plugins.begin(), _plugins.end(), plugin) != _plugins.end()) {
            return;
        }
        _plugins.push_back(plugin);
        plugin->onAttach(*this);
    }

    void unregisterPlugin(IHostPluginHooks* plugin) override
    {
        if (plugin == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(_pluginMutex);
        const auto it = std::find(_plugins.begin(), _plugins.end(), plugin);
        if (it == _plugins.end()) {
            return;
        }
        plugin->onDetach(*this);
        _plugins.erase(it);
    }

    ~DefaultEngineHost() override
    {
        // Detach remaining plugins without touching EventBus (safe teardown).
        std::lock_guard<std::mutex> lock(_pluginMutex);
        for (auto* p : _plugins) {
            if (p) {
                p->onDetach(*this);
            }
        }
        _plugins.clear();
    }

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::string, void*> _services;
    mutable std::mutex _pluginMutex;
    std::vector<IHostPluginHooks*> _plugins;
};

DefaultEngineHost g_defaultHost;

} // namespace

IEngineHost& defaultEngineHost()
{
    return g_defaultHost;
}

ayt::event::EventBus& resolveEventBus()
{
    if (auto* host = currentEngineHost()) {
        return host->eventBus();
    }
    return ayt::event::EventBus::instance();
}

namespace {

void postTaskCompleteToEventBus(const ayt::task::TaskCompletionNotice& n)
{
    ayt::event::TaskCompleteEvent ev{};
    ev.taskId = reinterpret_cast<uint64_t>(n.task);
    ev.ok = (n.task != nullptr) && !n.cancelled;
    ayt::event::EventBus::instance().post(ev);
}

} // namespace

void bindBuiltinHostServices(IEngineHost& host)
{
    host.provide(kHostServiceResources, &ayt::resource::ResourceManager::instance());

    if (auto* sub = dynamic_cast<ayt::audio::AudioSubSystem*>(host.findSubSystem("Audio"))) {
        if (auto* eng = sub->engine()) {
            host.provide(kHostServiceAudio, eng);
        }
    }
    AY_DEPRECATED_SUPPRESS_BEGIN
    host.provide(kHostServiceScenes, &ayt::scene::SceneManager::instance());
    AY_DEPRECATED_SUPPRESS_END

    // Scene lifecycle → EventBus (Host bridge; AYScene stays EventSystem-free).
    if (auto* sm = host.scenes()) {
        sm->setLifecycleObserver(&SceneLifecycleEventBridge::instance());
    }

    // AYTask completion → EventBus (Task stays EventSystem-free via hook).
    ayt::task::setTaskCompletionHook(&postTaskCompleteToEventBus);

    if (auto* physSub = ayt::physics::PhysicsSubSystem::findRegistered()) {
        if (auto* mgr = physSub->manager()) {
            providePhysics(host, mgr);
        }
        if (auto* query = physSub->query()) {
            providePhysicsQuery(host, query);
        }
    }
}

void providePhysics(IEngineHost& host, ayt::physics::PhysicsManager* manager)
{
    host.provide(kHostServicePhysics, manager);
}

void providePhysicsQuery(IEngineHost& host, ayt::physics::IPhysicsQuery* query)
{
    host.provide(kHostServicePhysicsQuery, query);
}

} // namespace ayt::app

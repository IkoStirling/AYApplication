#include <IEngineHost.h>

#include <AYAudioSubSystem.h>
#include <AYGameLoop.h>
#include <AYResourceManager.h>
#include <AYSubSystemRegistry.h>
#include <ayevent/EventBus.h>

#include <mutex>
#include <string>
#include <unordered_map>

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
        return static_cast<ayt::physics::PhysicsManager*>(
            findService(kHostServicePhysics));
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

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::string, void*> _services;
};

DefaultEngineHost g_defaultHost;

} // namespace

IEngineHost& defaultEngineHost()
{
    return g_defaultHost;
}

void bindBuiltinHostServices(IEngineHost& host)
{
    host.provide(kHostServiceResources, &ayt::resource::ResourceManager::instance());

    if (auto* sub = dynamic_cast<ayt::audio::AudioSubSystem*>(host.findSubSystem("Audio"))) {
        if (auto* eng = sub->engine()) {
            host.provide(kHostServiceAudio, eng);
        }
    }
    // PhysicsManager is not a process singleton — call sites provide when created:
    //   host.provide(kHostServicePhysics, physicsManager.get());
}

} // namespace ayt::app

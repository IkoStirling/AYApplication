#include <IEngineHost.h>

#include <AYAudioSubSystem.h>
#include <AYGameLoop.h>
#include <AYResourceManager.h>
#include <AYSceneManager.h>  // PR-6 (v0.1.3): scenes() facade
#include <AYSubSystemRegistry.h>
#include <DeprecatedSuppress.h>  // v0.3 PR-4 (AYScene): instance() [[deprecated]] 豁免
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

    // PR-6 (v0.1.3): scenes() facade — key+fallback 双路径（mirror resources()）。
    // findService(kHostServiceScenes) 若找到返之；否则 fallback 到 singleton。
    // clearProvidedServices() 后 service<T>(key) 返 nullptr，但 scenes() 仍 OK
    // （与 resources() 行为对齐）。
    //
    // v0.3 PR-4 (AYScene): fallback `instance()` 触发 [[deprecated]] warning —
    // 豁免（AYSceneManager.h 注释）；facade 自身就靠 instance 取 singleton
    // （circular：facade 是 host->scenes() 的实现，不能再调 facade）。
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
    // PR-6 (v0.1.3, design §10 Q-F 收口): 关卡生命周期管家注册。
    // v0.3 PR-4: instance() [[deprecated]] 豁免（bindBuiltinHostServices 是
    // facade 提供者，自身就靠 instance 取 singleton — circular）。
    AY_DEPRECATED_SUPPRESS_BEGIN
    host.provide(kHostServiceScenes, &ayt::scene::SceneManager::instance());
    AY_DEPRECATED_SUPPRESS_END

    // PhysicsManager is not a process singleton — call sites provide when created:
    //   host.provide(kHostServicePhysics, physicsManager.get());
}

} // namespace ayt::app

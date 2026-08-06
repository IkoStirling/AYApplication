#pragma once
// IEngineHost.h — engine shell: assembly lookup + typed/keyed services
//
// Built-in accessors (resources/physics/audio) are sugar over provideService.
// New singletons: register a stable key — see docs/engine-host.md §3–§4.

#include <IAYGameLoop.h>

#include <string_view>

namespace ayt::event
{
class EventBus;
}

namespace ayt::resource
{
class ResourceManager;
}

namespace ayt::physics
{
class PhysicsManager;
}

namespace ayt::audio
{
class AudioEngine;
}

namespace ayt::scene
{
class SceneManager;
}

namespace ayt::app
{

// ---------------------------------------------------------------------------
// Stable service keys (add new keys here + docs/engine-host.md table together)
// ---------------------------------------------------------------------------
inline constexpr const char* kHostServiceResources = "ayt.resource.ResourceManager";
inline constexpr const char* kHostServicePhysics   = "ayt.physics.PhysicsManager";
inline constexpr const char* kHostServiceAudio     = "ayt.audio.AudioEngine";
// PR-6 (v0.1.3, design §10 Q-F 收口): 关卡生命周期管家。
inline constexpr const char* kHostServiceScenes    = "ayt.scene.SceneManager";

/// Process-scoped engine host: assembly + service discovery (not a gameplay module).
class IEngineHost {
public:
    virtual ~IEngineHost() = default;

    // --- Always-on shell ---
    virtual ayt::game::IGameLoop& gameLoop() = 0;
    virtual ayt::event::EventBus& eventBus() = 0;

    /// Escape hatch for GameLoop subsystems. Prefer typed services below.
    virtual ayt::game::ISubSystem* findSubSystem(const char* name) = 0;

    // --- Service registry (extensible) ---
    /// Register or replace a service pointer for `key` (does not take ownership).
    /// Pass nullptr to clear. Keys should be stable string literals (see kHostService*).
    virtual void provideService(std::string_view key, void* instance) = 0;
    virtual void* findService(std::string_view key) const = 0;
    virtual void clearProvidedServices() = 0;

    template <typename T>
    void provide(std::string_view key, T* instance)
    {
        provideService(key, static_cast<void*>(instance));
    }

    template <typename T>
    T* service(std::string_view key) const
    {
        return static_cast<T*>(findService(key));
    }

    // --- Built-in typed accessors (nullptr if not provided / not ready) ---
    /// L2 resource manager. Default host falls back to ResourceManager::instance().
    virtual ayt::resource::ResourceManager* resources() = 0;
    /// Physics manager when the app/game has provide()'d one (not a process singleton today).
    virtual ayt::physics::PhysicsManager* physics() = 0;
    /// Audio engine when AudioSubSystem is registered and initialized; else provided pointer.
    virtual ayt::audio::AudioEngine* audio() = 0;
    /// 关卡生命周期管家（design §10 Q-F 收口；v0.1.3）。
    /// Meyers singleton — **永不为 null**（与 physics()/audio() 的「未 provide 则 nullptr」
    /// 语义不同；SM 进程内必存在）。替代访问路径：
    /// `host.service<ayt::scene::SceneManager>(kHostServiceScenes)`。
    /// 提供 process-wide 的 current / setEdit / beginPlay / endPlay / tick / 诊断字段
    ///（详见 `AYSceneManager.h`）。
    virtual ayt::scene::SceneManager* scenes() = 0;
};

/// Default process-wide host.
IEngineHost& defaultEngineHost();

/// Host installed for the active Application::run (nullptr outside scope).
IEngineHost* currentEngineHost();

void setCurrentEngineHost(IEngineHost* host);

/// Wire well-known builtins into `host` (resources singleton; audio/physics if available).
/// Call after default module registration (and again after GameLoop init if audio was null).
void bindBuiltinHostServices(IEngineHost& host);

/// RAII: sets currentEngineHost for the duration of Application::run.
class EngineHostScope {
public:
    explicit EngineHostScope(IEngineHost& host);
    ~EngineHostScope();

    EngineHostScope(const EngineHostScope&) = delete;
    EngineHostScope& operator=(const EngineHostScope&) = delete;

private:
    IEngineHost* _previous = nullptr;
};

} // namespace ayt::app

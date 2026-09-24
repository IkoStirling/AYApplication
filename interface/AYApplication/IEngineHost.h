#pragma once
// AYApplication/IEngineHost.h — engine shell: assembly lookup + typed/keyed services
//
// Built-in accessors (resources/physics/audio) are sugar over provideService.
// New singletons: register a stable key — see docs/engine-host.md §3–§4.

#include <AYGameLoop/IGameLoop.h>
#include <AYApplication/IHostPluginHooks.h>

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
class IPhysicsQuery;
}

namespace ayt::audio
{
class AudioEngine;
}

namespace ayt::device
{
class DeviceManager;
}

namespace ayt::scene
{
class SceneManager;
}

namespace ayt::app
{

class SaveGameService;

// ---------------------------------------------------------------------------
// Stable service keys (add new keys here + docs/engine-host.md table together)
// ---------------------------------------------------------------------------
inline constexpr const char* kHostServiceResources    = "ayt.resource.ResourceManager";
inline constexpr const char* kHostServicePhysics      = "ayt.physics.PhysicsManager";
inline constexpr const char* kHostServicePhysicsQuery = "ayt.physics.IPhysicsQuery";
inline constexpr const char* kHostServiceAudio        = "ayt.audio.AudioEngine";
inline constexpr const char* kHostServiceDeviceManager = "ayt.device.DeviceManager";
// PR-6 (v0.1.3, design §10 Q-F 收口): 关卡生命周期管家。
inline constexpr const char* kHostServiceScenes       = "ayt.scene.SceneManager";
inline constexpr const char* kHostServiceRuntimeSceneLoader =
    "ayt.app.RuntimeSceneLoader";
inline constexpr const char* kHostServiceGameWorldRouter =
    "ayt.app.GameWorldRouter";
inline constexpr const char* kHostServiceGameFlowRuntime =
    "ayt.app.GameFlowRuntime";
inline constexpr const char* kHostServiceGameFlowUIBridge =
    "ayt.app.GameFlowUIBridge";
inline constexpr const char* kHostServiceUIFlowRuntime =
    "ayt.app.UIFlowRuntime";
inline constexpr const char* kHostServiceUIFlowSceneBridge =
    "ayt.app.UIFlowSceneBridge";
inline constexpr const char* kHostServiceSaveGame =
    "ayt.app.SaveGameService";

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
    /// Physics manager when provided / PhysicsSubSystem initialized.
    virtual ayt::physics::PhysicsManager* physics() = 0;
    /// Narrow query facade (raycast / overlap). Prefer over physics() when possible.
    virtual ayt::physics::IPhysicsQuery* physicsQuery() = 0;
    /// Audio engine when AudioSubSystem is registered and initialized; else provided pointer.
    virtual ayt::audio::AudioEngine* audio() = 0;
    /// 关卡生命周期管家（design §10 Q-F 收口；v0.1.3）。
    /// Meyers singleton — **永不为 null**（与 physics()/audio() 的「未 provide 则 nullptr」
    /// 语义不同；SM 进程内必存在）。替代访问路径：
    /// `host.service<ayt::scene::SceneManager>(kHostServiceScenes)`。
    /// 提供 process-wide 的 current / setEdit / beginPlay / endPlay / tick / 诊断字段
    ///（详见 `AYScene/SceneManager.h`）。
    virtual ayt::scene::SceneManager* scenes() = 0;

    // --- Plugin extension points (default no-op; DefaultEngineHost stores hooks) ---
    virtual void registerPlugin(IHostPluginHooks* plugin) { (void)plugin; }
    virtual void unregisterPlugin(IHostPluginHooks* plugin) { (void)plugin; }
};

/// Default process-wide host.
IEngineHost& defaultEngineHost();

/// Host installed for the active Application::run (nullptr outside scope).
IEngineHost* currentEngineHost();

void setCurrentEngineHost(IEngineHost* host);

/// Prefer current Host's eventBus; fall back to EventBus::instance().
ayt::event::EventBus& resolveEventBus();

/// Wire well-known builtins into `host` (resources singleton; audio/physics if available).
/// This is a refresh-only compatibility entry point. Runtime owners should use
/// EngineRuntimeScope so services, process hooks, Scene selection and the active
/// World are restored together. Call again after initialization if audio was null.
void bindBuiltinHostServices(IEngineHost& host);

/// P1: register a PhysicsManager into the host service table (`kHostServicePhysics`).
/// Physics is not a process singleton — call after `PhysicsManager::create`.
void providePhysics(IEngineHost& host, ayt::physics::PhysicsManager* manager);

/// Register narrow IPhysicsQuery (`kHostServicePhysicsQuery`).
void providePhysicsQuery(IEngineHost& host, ayt::physics::IPhysicsQuery* query);

/**
 * @brief Resolves the Host-owned device and logical-input service.
 * @param host Active engine Host whose service table is queried.
 * @return Borrowed manager, or nullptr when the Device runtime is not installed.
 * @ownership
 * The Host does not own the returned manager. Do not retain it across Host or
 * module shutdown; call DeviceManager::isInitialized() before consuming input.
 * @threading
 * Resolve and use the manager from the game/UI thread.
 */
inline ayt::device::DeviceManager* deviceManager(IEngineHost& host) noexcept
{
    return host.service<ayt::device::DeviceManager>(kHostServiceDeviceManager);
}

/**
 * @brief Publishes or clears the non-owning DeviceManager Host service.
 * @param host Host receiving the service pointer.
 * @param manager Borrowed manager; pass nullptr to clear the service.
 */
void provideDeviceManager(
    IEngineHost& host,
    ayt::device::DeviceManager* manager);

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

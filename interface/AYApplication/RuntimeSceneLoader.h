#pragma once
// Frame-boundary runtime Scene loading owned by AYApplication.

#include <AYEventSystem/EventPriority.h>
#include <AYGameLoop/IGameLoop.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace ayt::event { class EventBus; }
namespace ayt::scene { class Scene; class SceneManager; }

namespace ayt::app
{

enum class RuntimeSceneLoadState : uint8_t {
    Idle,
    Queued,
    Ready,
    Failed,
    Cancelled,
};

struct RuntimeSceneLoadRequest {
    uint64_t requestId = 0;
    std::string scenePath;
    std::string sceneName;

    // Runs against the fully loaded staging Scene before it becomes current.
    // Returning false preserves the old current Scene and reports failure.
    std::function<bool(::ayt::scene::Scene&, std::string&)> prepareActivation;

    bool isValid() const;
};

struct RuntimeSceneLoadStatus {
    RuntimeSceneLoadState state = RuntimeSceneLoadState::Idle;
    uint64_t requestId = 0;
    std::string scenePath;
    std::string message;
};

struct RuntimeSceneLoaderConfig {
    std::string initialSceneName = "Client";
    std::string initialScenePath;

    // Invoked after SceneManager::current() has changed. Standalone games use
    // this hook to bind systems that resolve World::instance() at bootstrap.
    std::function<void(::ayt::scene::Scene&)> onSceneActivated;
};

struct RuntimeSceneLoadFinishedEvent {
    static constexpr ::ayt::event::EventTypeId kTypeId = 0x0009'0001u;
    static constexpr ::ayt::event::EventPriority kPriority =
        ::ayt::event::EventPriority::High;

    uint64_t requestId = 0;
    bool success = false;
};
static_assert(std::is_trivially_copyable_v<RuntimeSceneLoadFinishedEvent>);

class IRuntimeSceneLoader : public ::ayt::game::ISubSystem {
public:
    ~IRuntimeSceneLoader() override = default;

    // Request ids are caller-owned and must increase monotonically. Only one
    // request may be pending; callers must cancel it before submitting another.
    virtual bool requestLoad(RuntimeSceneLoadRequest request) = 0;
    virtual bool cancelLoad(uint64_t requestId) = 0;
    virtual RuntimeSceneLoadStatus getLoadStatus() const = 0;

    virtual ::ayt::scene::Scene* currentScene() = 0;
    virtual const ::ayt::scene::Scene* currentScene() const = 0;
};

std::unique_ptr<IRuntimeSceneLoader> createRuntimeSceneLoader(
    ::ayt::scene::SceneManager& scenes,
    RuntimeSceneLoaderConfig config = {},
    ::ayt::event::EventBus* eventBus = nullptr);

// Registration is idempotent. The GameLoop owns the returned subsystem.
bool registerRuntimeSceneLoader(
    ::ayt::scene::SceneManager& scenes,
    RuntimeSceneLoaderConfig config = {},
    ::ayt::event::EventBus* eventBus = nullptr);
IRuntimeSceneLoader* findRegisteredRuntimeSceneLoader();

} // namespace ayt::app

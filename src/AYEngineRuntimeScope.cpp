#include <AYApplication/EngineRuntimeScope.h>

#include <AYApplication/DeprecatedSuppress.h>
#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYEntity/World.h>
#include <AYScene/SceneManager.h>
#include <AYTask/TaskCompletionHook.h>

#include <array>
#include <string_view>
#include <utility>

namespace ayt::app
{

namespace
{

constexpr std::array<std::string_view, 13> kBuiltinServiceKeys{
    kHostServiceResources,
    kHostServicePhysics,
    kHostServicePhysicsQuery,
    kHostServiceAudio,
    kHostServiceDeviceManager,
    kHostServiceScenes,
    kHostServiceRuntimeSceneLoader,
    kHostServiceGameWorldRouter,
    kHostServiceGameFlowRuntime,
    kHostServiceGameFlowUIBridge,
    kHostServiceUIFlowRuntime,
    kHostServiceUIFlowSceneBridge,
    kHostServiceSaveGame,
};

} // namespace

class EngineRuntimeScope::Impl final
{
public:
    explicit Impl(IEngineHost& host)
        : host(host),
          previousTaskHook(ayt::task::getTaskCompletionHook())
    {
        for (std::size_t i = 0; i < kBuiltinServiceKeys.size(); ++i) {
            previousServices[i] = host.findService(kBuiltinServiceKeys[i]);
        }

        // bindBuiltinHostServices publishes the process SceneManager. Capture
        // its complete selection state first so World::instance() can be
        // restored together with the service table.
        AY_DEPRECATED_SUPPRESS_BEGIN
        sceneManager = &ayt::scene::SceneManager::instance();
        AY_DEPRECATED_SUPPRESS_END
        previousSceneObserver = sceneManager->lifecycleObserver();
        previousCurrentScene = sceneManager->current();
        previousEditScene = sceneManager->edit();
        previousPlayScene = sceneManager->play();
        previousActiveWorld = ayt::entity::World::activeWorld();

        bindBuiltinHostServices(host);
    }

    void refresh()
    {
        if (isActive) {
            bindBuiltinHostServices(host);
        }
    }

    void reset() noexcept
    {
        if (!isActive) {
            return;
        }
        isActive = false;

        // Play owns its World. Destroy it before restoring the borrowed Edit
        // and current pointers, then put the exact active-World redirect back.
        // Keep observers detached during restoration: leaving a scope is not a
        // new scene transition and may happen after event consumers shut down.
        if (sceneManager != nullptr) {
            sceneManager->setLifecycleObserver(nullptr);
            try {
                if (sceneManager->play() != previousPlayScene) {
                    sceneManager->endPlay();
                }
            } catch (...) {
                // Teardown cannot escape a host/application destructor.
            }
            sceneManager->setEdit(previousEditScene);
            sceneManager->setCurrent(previousCurrentScene);
            ayt::entity::World::setActiveWorld(previousActiveWorld);
            sceneManager->setLifecycleObserver(previousSceneObserver);
        }

        ayt::task::setTaskCompletionHook(previousTaskHook);

        for (std::size_t i = kBuiltinServiceKeys.size(); i-- > 0;) {
            try {
                host.provideService(kBuiltinServiceKeys[i], previousServices[i]);
            } catch (...) {
                // IEngineHost predates noexcept teardown. Continue restoring
                // the remaining independent slots if a custom host rejects one.
            }
        }
    }

    IEngineHost& host;
    std::array<void*, kBuiltinServiceKeys.size()> previousServices{};
    ayt::scene::SceneManager* sceneManager = nullptr;
    ayt::scene::ISceneLifecycleObserver* previousSceneObserver = nullptr;
    ayt::scene::Scene* previousCurrentScene = nullptr;
    ayt::scene::Scene* previousEditScene = nullptr;
    ayt::scene::Scene* previousPlayScene = nullptr;
    ayt::entity::World* previousActiveWorld = nullptr;
    ayt::task::TaskCompletionHook previousTaskHook = nullptr;
    bool isActive = true;
};

EngineRuntimeScope::EngineRuntimeScope(IEngineHost& host)
    : _impl(std::make_unique<Impl>(host))
{
}

EngineRuntimeScope::~EngineRuntimeScope()
{
    reset();
}

EngineRuntimeScope::EngineRuntimeScope(EngineRuntimeScope&&) noexcept = default;

EngineRuntimeScope& EngineRuntimeScope::operator=(
    EngineRuntimeScope&& other) noexcept
{
    if (this != &other) {
        reset();
        _impl = std::move(other._impl);
    }
    return *this;
}

void EngineRuntimeScope::refresh()
{
    if (_impl) {
        _impl->refresh();
    }
}

void EngineRuntimeScope::reset() noexcept
{
    if (_impl) {
        _impl->reset();
    }
}

bool EngineRuntimeScope::active() const noexcept
{
    return _impl && _impl->isActive;
}

} // namespace ayt::app

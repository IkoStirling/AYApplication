#include <AYApplication/RuntimeSceneLoader.h>

#include <AYEventSystem/EventBus.h>
#include <AYGameLoop/SubSystemRegistry.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>

#include <optional>
#include <utility>

namespace ayt::app
{
namespace
{

constexpr size_t kMaximumScenePathLength = 4096;
constexpr size_t kMaximumSceneNameLength = 256;

std::string describeLoadFailure(
    const std::string& path,
    const ::ayt::serializer::SerializeError& error) {
    std::string message = "Failed to load scene '" + path + "'";
    if (!error.message.empty()) message += ": " + error.message;
    if (!error.path.empty()) message += " at " + error.path;
    return message;
}

class RuntimeSceneLoader final : public IRuntimeSceneLoader {
public:
    RuntimeSceneLoader(::ayt::scene::SceneManager& scenes,
                       RuntimeSceneLoaderConfig config,
                       ::ayt::event::EventBus* eventBus)
        : _scenes(scenes),
          _config(std::move(config)),
          _eventBus(eventBus ? eventBus
                             : &::ayt::event::EventBus::instance()) {}

    const char* getName() const override { return "RuntimeSceneLoader"; }

    const ::ayt::game::SubSystemDescriptor& getDescriptor() const override {
        static const ::ayt::game::SubSystemDescriptor descriptor = {
            .name = "RuntimeSceneLoader",
            .dependencies = {},
            .basePriority = 1000,
            .timeType = ::ayt::game::SubSystemDescriptor::TimeType::Real,
            .phases = ::ayt::game::phaseBit(::ayt::game::FramePhase::Egress),
            .clock = ::ayt::game::ClockDomain::RealWall,
            .initializeAfter = {"Entity"},
            .runsAfter = {},
            .phasePriority = 1000,
            .reads = {"Application.SceneLoadRequest"},
            .writes = {"Scene.Current"},
        };
        return descriptor;
    }

    bool initialize() override {
        if (_current) return true;

        const std::string name = _config.initialSceneName.empty()
            ? "Client" : _config.initialSceneName;
        auto initial = std::make_unique<::ayt::scene::Scene>(
            ::ayt::scene::SceneMode::Play, name);

        _status = {};
        if (!_config.initialScenePath.empty()) {
            ::ayt::serializer::SerializeError error{};
            if (!initial->load(_config.initialScenePath, &error)) {
                _status.state = RuntimeSceneLoadState::Failed;
                _status.scenePath = _config.initialScenePath;
                _status.message = describeLoadFailure(
                    _config.initialScenePath, error);
            } else {
                _status.state = RuntimeSceneLoadState::Ready;
                _status.scenePath = _config.initialScenePath;
            }
        } else {
            _status.state = RuntimeSceneLoadState::Ready;
        }

        _current = std::move(initial);
        _scenes.setCurrent(_current.get());
        activateCurrent();
        return true;
    }

    void update(float) override {
        if (!_pending) return;

        RuntimeSceneLoadRequest request = std::move(*_pending);
        _pending.reset();

        auto staged = std::make_unique<::ayt::scene::Scene>(
            ::ayt::scene::SceneMode::Play,
            request.sceneName.empty() ? request.scenePath : request.sceneName);
        ::ayt::serializer::SerializeError error{};
        if (!staged->load(request.scenePath, &error)) {
            _status.state = RuntimeSceneLoadState::Failed;
            _status.requestId = request.requestId;
            _status.scenePath = request.scenePath;
            _status.message = describeLoadFailure(request.scenePath, error);
            publishFinished(request.requestId, false);
            return;
        }

        if (request.prepareActivation) {
            std::string message;
            if (!request.prepareActivation(*staged, message)) {
                _status.state = RuntimeSceneLoadState::Failed;
                _status.requestId = request.requestId;
                _status.scenePath = request.scenePath;
                _status.message = message.empty()
                    ? "Scene activation preparation failed"
                    : std::move(message);
                publishFinished(request.requestId, false);
                return;
            }
        }

        _scenes.setCurrent(staged.get());
        _current = std::move(staged);
        _status.state = RuntimeSceneLoadState::Ready;
        _status.requestId = request.requestId;
        _status.scenePath = request.scenePath;
        _status.message.clear();
        activateCurrent();
        publishFinished(request.requestId, true);
    }

    void fixedUpdate(float) override {}

    void shutdown() override {
        _pending.reset();
        if (_scenes.current() == _current.get()) {
            _scenes.setCurrent(nullptr);
        }
        _current.reset();
        _status = {};
        _lastAcceptedRequestId = 0;
    }

    bool requestLoad(RuntimeSceneLoadRequest request) override {
        if (_pending || !request.isValid() ||
            request.requestId <= _lastAcceptedRequestId) {
            return false;
        }
        _lastAcceptedRequestId = request.requestId;
        _status.state = RuntimeSceneLoadState::Queued;
        _status.requestId = request.requestId;
        _status.scenePath = request.scenePath;
        _status.message.clear();
        _pending = std::move(request);
        return true;
    }

    bool cancelLoad(uint64_t requestId) override {
        if (!_pending || requestId == 0 ||
            _pending->requestId != requestId) return false;
        _pending.reset();
        _status.state = RuntimeSceneLoadState::Cancelled;
        _status.requestId = requestId;
        _status.message = "Scene load cancelled";
        return true;
    }

    RuntimeSceneLoadStatus getLoadStatus() const override { return _status; }
    ::ayt::scene::Scene* currentScene() override { return _current.get(); }
    const ::ayt::scene::Scene* currentScene() const override {
        return _current.get();
    }

private:
    void activateCurrent() {
        if (_config.onSceneActivated && _current) {
            _config.onSceneActivated(*_current);
        }
    }

    void publishFinished(uint64_t requestId, bool success) {
        if (_eventBus) {
            _eventBus->post(RuntimeSceneLoadFinishedEvent{
                .requestId = requestId,
                .success = success,
            });
        }
    }

    ::ayt::scene::SceneManager& _scenes;
    RuntimeSceneLoaderConfig _config;
    ::ayt::event::EventBus* _eventBus = nullptr;
    std::unique_ptr<::ayt::scene::Scene> _current;
    std::optional<RuntimeSceneLoadRequest> _pending;
    RuntimeSceneLoadStatus _status;
    uint64_t _lastAcceptedRequestId = 0;
};

} // namespace

bool RuntimeSceneLoadRequest::isValid() const {
    return requestId != 0 && !scenePath.empty() &&
           scenePath.size() <= kMaximumScenePathLength &&
           sceneName.size() <= kMaximumSceneNameLength;
}

std::unique_ptr<IRuntimeSceneLoader> createRuntimeSceneLoader(
    ::ayt::scene::SceneManager& scenes,
    RuntimeSceneLoaderConfig config,
    ::ayt::event::EventBus* eventBus) {
    return std::make_unique<RuntimeSceneLoader>(
        scenes, std::move(config), eventBus);
}

IRuntimeSceneLoader* findRegisteredRuntimeSceneLoader() {
    auto* system = ::ayt::game::SubSystemRegistry::instance().findSubSystem(
        "RuntimeSceneLoader");
    return dynamic_cast<IRuntimeSceneLoader*>(system);
}

bool registerRuntimeSceneLoader(
    ::ayt::scene::SceneManager& scenes,
    RuntimeSceneLoaderConfig config,
    ::ayt::event::EventBus* eventBus) {
    if (findRegisteredRuntimeSceneLoader()) return true;
    auto system = createRuntimeSceneLoader(
        scenes, std::move(config), eventBus);
    ::ayt::game::IGameLoop::instance().registerSubSystem(system.release());
    return findRegisteredRuntimeSceneLoader() != nullptr;
}

} // namespace ayt::app

#include <AYOnlineApplication/OnlineApplication.h>

#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
#include <AYGameLoop/SubSystemRegistry.h>

#include <limits>
#include <utility>

namespace ayt::app::online
{
namespace
{

constexpr size_t kMaximumScenePathLength = 4096;
constexpr size_t kMaximumSceneNameLength = 256;

bool isMenuState(::ayt::net::OnlineFlowState state) {
    return state == ::ayt::net::OnlineFlowState::MainMenu ||
           state == ::ayt::net::OnlineFlowState::SignedOut ||
           state == ::ayt::net::OnlineFlowState::Failed;
}

} // namespace

bool OnlineSceneBridgeConfig::isValid() const {
    return contentResolver != nullptr && !mainMenuScenePath.empty() &&
           mainMenuScenePath.size() <= kMaximumScenePathLength &&
           mainMenuSceneName.size() <= kMaximumSceneNameLength;
}

struct OnlineSceneBridge::Impl {
    enum class PendingPurpose : uint8_t { None, Session, MainMenu };

    Impl(::ayt::net::OnlineFlowCoordinator& flowValue,
         ::ayt::app::IRuntimeSceneLoader& loaderValue,
         OnlineSceneBridgeConfig configValue,
         ::ayt::event::EventBus& eventBusValue)
        : flow(flowValue),
          loader(loaderValue),
          config(std::move(configValue)),
          eventBus(eventBusValue) {}

    bool initialize() {
        if (status.ready) return true;
        if (!config.isValid()) {
            status.lastError = "Online scene bridge configuration is invalid";
            return false;
        }
        const auto loaderStatus = loader.getLoadStatus();
        nextSceneRequestId = loaderStatus.requestId;
        loadConnection = eventBus.subscribe<::ayt::net::OnlineFlowLoadRequestedEvent>(
            [this](const auto& event) { onLoadRequested(event); });
        statusConnection = eventBus.subscribe<::ayt::net::OnlineFlowStatusChangedEvent>(
            [this](const auto& event) { onFlowStatusChanged(event); });
        sceneConnection = eventBus.subscribe<::ayt::app::RuntimeSceneLoadFinishedEvent>(
            [this](const auto& event) { onSceneLoadFinished(event); });
        sceneCurrentConnection =
            eventBus.subscribe<::ayt::event::SceneCurrentChangedEvent>(
                [this](const auto& event) { onSceneCurrentChanged(event); });
        status.ready = true;
        return true;
    }

    void update() {
        if (!status.ready) return;
        const auto flowStatus = flow.getStatus();
        if (isMenuState(flowStatus.state) && status.sessionSceneActive &&
            !status.mainMenuRecoveryRequired &&
            pendingPurpose == PendingPurpose::None) {
            (void)requestMainMenu();
        }
    }

    bool retryMainMenuScene() {
        if (!status.ready || !status.sessionSceneActive ||
            pendingPurpose != PendingPurpose::None ||
            !isMenuState(flow.getStatus().state)) return false;
        status.mainMenuRecoveryRequired = false;
        if (requestMainMenu()) return true;
        status.mainMenuRecoveryRequired = true;
        return false;
    }

    void shutdown() {
        deactivateActiveSessionScene();
        if (loadConnection != 0) eventBus.unsubscribe(loadConnection);
        if (statusConnection != 0) eventBus.unsubscribe(statusConnection);
        if (sceneConnection != 0) eventBus.unsubscribe(sceneConnection);
        if (sceneCurrentConnection != 0) {
            eventBus.unsubscribe(sceneCurrentConnection);
        }
        loadConnection = 0;
        statusConnection = 0;
        sceneConnection = 0;
        sceneCurrentConnection = 0;
        if (pendingPurpose != PendingPurpose::None) {
            (void)loader.cancelLoad(status.sceneRequestId);
        }
        pendingPurpose = PendingPurpose::None;
        pendingContent = {};
        status = {};
    }

    bool allocateRequestId(uint64_t& output) {
        const uint64_t observed = loader.getLoadStatus().requestId;
        nextSceneRequestId = (std::max)(nextSceneRequestId, observed);
        if (nextSceneRequestId ==
            (std::numeric_limits<uint64_t>::max)()) {
            status.lastError = "Runtime scene request id exhausted";
            return false;
        }
        output = ++nextSceneRequestId;
        return true;
    }

    void clearPending() {
        pendingPurpose = PendingPurpose::None;
        pendingContent = {};
        status.loadPending = false;
        status.flowGeneration = 0;
    }

    void failFlowLoad(uint64_t generation, std::string message) {
        status.lastError = message;
        (void)flow.failLoading(generation, std::move(message));
    }

    bool isCurrentFlowEvent(
        const ::ayt::net::OnlineFlowStatusChangedEvent& event) const {
        const auto current = flow.getStatus();
        return event.state == current.state &&
               event.sessionState == current.sessionState &&
               event.loadingGeneration == current.loadingGeneration &&
               event.sessionId == current.sessionId &&
               event.sessionEpoch == current.sessionEpoch &&
               event.worldLoaded == current.worldLoaded;
    }

    void suspendActiveSessionScene(bool recovery) {
        if (!status.sessionSceneActive || status.sessionSceneSuspended ||
            status.sessionSceneDeactivated) return;
        if (recovery) ++status.recoveryGeneration;
        if (config.suspendSessionScene && activeScene) {
            config.suspendSessionScene(*activeScene);
        }
        status.sessionSceneSuspended = true;
    }

    void deactivateActiveSessionScene() {
        if (!status.sessionSceneActive || status.sessionSceneDeactivated) {
            return;
        }
        suspendActiveSessionScene(false);
        if (config.deactivateSessionScene && activeScene) {
            config.deactivateSessionScene(*activeScene);
        }
        status.sessionSceneDeactivated = true;
    }

    void failActiveWorld(std::string message) {
        status.lastError = message;
        const auto current = flow.getStatus();
        if (current.state == ::ayt::net::OnlineFlowState::LoadingSession &&
            activeFlowGeneration != 0) {
            (void)flow.failLoading(activeFlowGeneration, std::move(message));
        } else if (current.state == ::ayt::net::OnlineFlowState::InSession) {
            (void)flow.failActiveSession(std::move(message));
        }
    }

    bool validateRecoveredSession(
        const ::ayt::net::OnlineFlowStatusChangedEvent& event) {
        if (status.activeSessionId != 0 && event.sessionId != 0 &&
            status.activeSessionId != event.sessionId) {
            failActiveWorld("Recovered transport belongs to another session");
            return false;
        }
        if (status.activeSessionEpoch != 0 && event.sessionEpoch != 0 &&
            event.sessionEpoch < status.activeSessionEpoch) {
            failActiveWorld("Recovered transport has a stale authority epoch");
            return false;
        }
        return true;
    }

    bool resumeActiveSessionScene(
        const ::ayt::net::OnlineFlowStatusChangedEvent& event) {
        if (!validateRecoveredSession(event)) return false;
        if (status.sessionSceneDeactivated) return false;

        const bool epochAdvanced = status.activeSessionEpoch != 0 &&
            event.sessionEpoch > status.activeSessionEpoch;
        if (epochAdvanced && !status.sessionSceneSuspended) {
            suspendActiveSessionScene(true);
        }
        if (status.sessionSceneSuspended) {
            std::string message;
            if (config.resumeSessionScene && activeScene &&
                !config.resumeSessionScene(
                    *activeScene, status.recoveryGeneration,
                    event.sessionEpoch, message)) {
                failActiveWorld(message.empty()
                    ? "Session scene recovery callback failed"
                    : std::move(message));
                return false;
            }
            status.sessionSceneSuspended = false;
        }
        if (event.sessionId != 0) status.activeSessionId = event.sessionId;
        if (event.sessionEpoch != 0) {
            status.activeSessionEpoch = event.sessionEpoch;
        }
        return true;
    }

    void onLoadRequested(
        const ::ayt::net::OnlineFlowLoadRequestedEvent& event) {
        const auto flowStatus = flow.getStatus();
        if (event.generation == 0 ||
            flowStatus.state != ::ayt::net::OnlineFlowState::LoadingSession ||
            flowStatus.loadingGeneration != event.generation) {
            return;
        }
        if (pendingPurpose == PendingPurpose::Session &&
            status.flowGeneration == event.generation) {
            return;
        }
        if (pendingPurpose != PendingPurpose::None) {
            failFlowLoad(event.generation,
                         "Another runtime scene load is already pending");
            return;
        }
        if (!flowStatus.content.isValid()) {
            failFlowLoad(event.generation,
                         "Session assignment has no valid content identity");
            return;
        }

        auto resolved = config.contentResolver->resolve(flowStatus.content);
        if (!resolved.isValid()) {
            failFlowLoad(event.generation,
                resolved.message.empty()
                    ? "Online content could not be resolved locally"
                    : std::move(resolved.message));
            return;
        }

        uint64_t requestId = 0;
        if (!allocateRequestId(requestId)) {
            failFlowLoad(event.generation, status.lastError);
            return;
        }

        const auto content = flowStatus.content;
        ::ayt::app::RuntimeSceneLoadRequest request;
        request.requestId = requestId;
        request.scenePath = resolved.scenePath;
        request.sceneName = resolved.sceneName.empty()
            ? content.contentId : resolved.sceneName;
        if (config.prepareSessionScene) {
            auto prepare = config.prepareSessionScene;
            request.prepareActivation =
                [prepare = std::move(prepare), content](
                    ::ayt::scene::Scene& scene, std::string& message) {
                    return prepare(content, scene, message);
                };
        }
        if (!loader.requestLoad(std::move(request))) {
            failFlowLoad(event.generation,
                         "Runtime scene loader rejected the session scene");
            return;
        }

        pendingPurpose = PendingPurpose::Session;
        pendingContent = content;
        status.loadPending = true;
        status.sceneRequestId = requestId;
        status.flowGeneration = event.generation;
        status.lastError.clear();
    }

    bool requestMainMenu() {
        if (pendingPurpose != PendingPurpose::None) return false;
        uint64_t requestId = 0;
        if (!allocateRequestId(requestId)) return false;
        deactivateActiveSessionScene();
        ::ayt::app::RuntimeSceneLoadRequest request;
        request.requestId = requestId;
        request.scenePath = config.mainMenuScenePath;
        request.sceneName = config.mainMenuSceneName;
        if (!loader.requestLoad(std::move(request))) {
            status.lastError = "Runtime scene loader rejected the main menu";
            status.mainMenuRecoveryRequired = true;
            return false;
        }
        pendingPurpose = PendingPurpose::MainMenu;
        status.loadPending = true;
        status.sceneRequestId = requestId;
        status.flowGeneration = 0;
        status.lastError.clear();
        return true;
    }

    void onFlowStatusChanged(
        const ::ayt::net::OnlineFlowStatusChangedEvent& event) {
        // Event delivery can lag behind multiple coordinator updates. Only
        // the current snapshot may drive destructive scene lifecycle work.
        if (!isCurrentFlowEvent(event)) return;

        // Any transition away from the exact loading generation makes the
        // staged request stale.  In particular, LoadingTimedOut enters Failed
        // without passing through Leaving; allowing the queued request to run
        // afterwards would briefly activate a world the flow has rejected.
        if (pendingPurpose == PendingPurpose::Session &&
            (event.state != ::ayt::net::OnlineFlowState::LoadingSession ||
             event.loadingGeneration != status.flowGeneration)) {
            (void)loader.cancelLoad(status.sceneRequestId);
            clearPending();
        }

        if (status.sessionSceneActive) {
            if (event.state == ::ayt::net::OnlineFlowState::InSession &&
                event.sessionState ==
                    ::ayt::net::OnlineSessionCoordinatorState::Connecting) {
                suspendActiveSessionScene(true);
            } else if (event.state == ::ayt::net::OnlineFlowState::InSession &&
                       event.sessionState ==
                         ::ayt::net::OnlineSessionCoordinatorState::InSession) {
                if (!resumeActiveSessionScene(event)) return;
            } else if (event.state == ::ayt::net::OnlineFlowState::Leaving ||
                       event.state ==
                         ::ayt::net::OnlineFlowState::SigningOut) {
                suspendActiveSessionScene(false);
            }
        }
        if (isMenuState(event.state) && status.sessionSceneActive &&
            pendingPurpose == PendingPurpose::None) {
            (void)requestMainMenu();
        }
    }

    void onSceneCurrentChanged(
        const ::ayt::event::SceneCurrentChangedEvent& event) {
        if (!status.sessionSceneActive || event.current == activeScene) return;

        // RuntimeSceneLoader publishes current-changed before its finished
        // event. A matching pending request is an expected atomic replacement.
        if (pendingPurpose != PendingPurpose::None &&
            event.current == loader.currentScene()) return;

        suspendActiveSessionScene(false);
        deactivateActiveSessionScene();
        failActiveWorld("Active session scene was replaced outside the "
                        "online scene loader");
        if (isMenuState(flow.getStatus().state) &&
            pendingPurpose == PendingPurpose::None) {
            (void)requestMainMenu();
        }
    }

    void onSceneLoadFinished(
        const ::ayt::app::RuntimeSceneLoadFinishedEvent& event) {
        if (pendingPurpose == PendingPurpose::None ||
            event.requestId != status.sceneRequestId) return;

        const PendingPurpose purpose = pendingPurpose;
        const uint64_t flowGeneration = status.flowGeneration;
        const auto content = pendingContent;
        const auto loaderStatus = loader.getLoadStatus();
        clearPending();

        if (!event.success) {
            status.lastError = loaderStatus.message.empty()
                ? "Runtime scene load failed" : loaderStatus.message;
            if (purpose == PendingPurpose::Session) {
                failFlowLoad(flowGeneration, status.lastError);
            } else {
                status.mainMenuRecoveryRequired = true;
            }
            return;
        }

        status.activeScenePath = loaderStatus.scenePath;
        status.lastError.clear();
        if (purpose == PendingPurpose::Session) {
            activeScene = loader.currentScene();
            if (!activeScene) {
                failFlowLoad(flowGeneration,
                             "Runtime scene loader activated no Scene");
                return;
            }
            status.sessionSceneActive = true;
            status.sessionSceneSuspended = false;
            status.sessionSceneDeactivated = false;
            status.mainMenuRecoveryRequired = false;
            status.activeContent = content;
            status.recoveryGeneration = 0;
            activeFlowGeneration = flowGeneration;
            const auto currentFlow = flow.getStatus();
            status.activeSessionId = currentFlow.sessionId;
            status.activeSessionEpoch = currentFlow.sessionEpoch;
            if (currentFlow.sessionState !=
                ::ayt::net::OnlineSessionCoordinatorState::InSession) {
                suspendActiveSessionScene(false);
            }
            if (currentFlow.state == ::ayt::net::OnlineFlowState::LoadingSession &&
                currentFlow.loadingGeneration == flowGeneration) {
                (void)flow.completeLoading(flowGeneration);
            }
        } else {
            status.sessionSceneActive = false;
            status.sessionSceneSuspended = false;
            status.sessionSceneDeactivated = false;
            status.mainMenuRecoveryRequired = false;
            status.activeContent = {};
            status.recoveryGeneration = 0;
            status.activeSessionId = 0;
            status.activeSessionEpoch = 0;
            activeFlowGeneration = 0;
            activeScene = nullptr;
        }
    }

    ::ayt::net::OnlineFlowCoordinator& flow;
    ::ayt::app::IRuntimeSceneLoader& loader;
    OnlineSceneBridgeConfig config;
    ::ayt::event::EventBus& eventBus;
    ::ayt::event::ConnectionId loadConnection = 0;
    ::ayt::event::ConnectionId statusConnection = 0;
    ::ayt::event::ConnectionId sceneConnection = 0;
    ::ayt::event::ConnectionId sceneCurrentConnection = 0;
    uint64_t nextSceneRequestId = 0;
    uint64_t activeFlowGeneration = 0;
    PendingPurpose pendingPurpose = PendingPurpose::None;
    ::ayt::net::OnlineContentDescriptor pendingContent;
    ::ayt::scene::Scene* activeScene = nullptr;
    OnlineSceneBridgeStatus status;
};

OnlineSceneBridge::OnlineSceneBridge(
    ::ayt::net::OnlineFlowCoordinator& flow,
    ::ayt::app::IRuntimeSceneLoader& loader,
    OnlineSceneBridgeConfig config,
    ::ayt::event::EventBus& eventBus)
    : _impl(std::make_unique<Impl>(
          flow, loader, std::move(config), eventBus)) {}

OnlineSceneBridge::~OnlineSceneBridge() { _impl->shutdown(); }
bool OnlineSceneBridge::initialize() { return _impl->initialize(); }
void OnlineSceneBridge::update() { _impl->update(); }
bool OnlineSceneBridge::retryMainMenuScene() {
    return _impl->retryMainMenuScene();
}
void OnlineSceneBridge::shutdown() { _impl->shutdown(); }
OnlineSceneBridgeStatus OnlineSceneBridge::getStatus() const {
    return _impl->status;
}

bool OnlineApplicationConfig::isValid() const {
    return online.isValid() && flow.isValid() && scenes.isValid();
}

namespace
{

class OnlineApplicationSubSystem final : public IOnlineApplicationSubSystem {
public:
    OnlineApplicationSubSystem(::ayt::app::IEngineHost& host,
                               ::ayt::net::IOnlineFlowSubSystem& flowSubSystem,
                               OnlineSceneBridgeConfig config)
        : _host(host),
          _flowSubSystem(flowSubSystem),
          _config(std::move(config)) {}

    const char* getName() const override { return "OnlineApplication"; }

    const ::ayt::game::SubSystemDescriptor& getDescriptor() const override {
        static const ::ayt::game::SubSystemDescriptor descriptor = {
            .name = "OnlineApplication",
            .dependencies = {},
            .basePriority = 110,
            .timeType = ::ayt::game::SubSystemDescriptor::TimeType::Real,
            .phases = ::ayt::game::phaseBit(::ayt::game::FramePhase::Ingress),
            .clock = ::ayt::game::ClockDomain::RealWall,
            .initializeAfter = {"OnlineFlow", "RuntimeSceneLoader"},
            .runsAfter = {"OnlineFlow"},
            .phasePriority = 110,
            .reads = {"Application.OnlineFlow", "Scene.Current"},
            .writes = {"Application.SceneLoadRequest"},
        };
        return descriptor;
    }

    bool initialize() override {
        if (_bridge) return true;
        auto* loader = _host.service<::ayt::app::IRuntimeSceneLoader>(
            ::ayt::app::kHostServiceRuntimeSceneLoader);
        if (!_flowSubSystem.isReady() ||
            !_flowSubSystem.coordinator() || !loader) return false;
        _flow = _flowSubSystem.coordinator();
        _bridge = std::make_unique<OnlineSceneBridge>(
            *_flow, *loader, _config, _host.eventBus());
        if (!_bridge->initialize()) {
            _bridge.reset();
            _flow = nullptr;
            return false;
        }
        _host.provide(kHostServiceOnlineFlow, _flow);
        _host.provide(kHostServiceOnlineApplication, this);
        return true;
    }

    void update(float) override {
        if (_bridge) _bridge->update();
    }
    void fixedUpdate(float) override {}

    void shutdown() override {
        if (_host.service<IOnlineApplicationSubSystem>(
                kHostServiceOnlineApplication) == this) {
            _host.provide<IOnlineApplicationSubSystem>(
                kHostServiceOnlineApplication, nullptr);
        }
        if (_host.service<::ayt::net::OnlineFlowCoordinator>(
                kHostServiceOnlineFlow) == _flow) {
            _host.provide<::ayt::net::OnlineFlowCoordinator>(
                kHostServiceOnlineFlow, nullptr);
        }
        if (_bridge) _bridge->shutdown();
        _bridge.reset();
        _flow = nullptr;
    }

    bool isReady() const override { return _bridge != nullptr; }
    ::ayt::net::OnlineFlowCoordinator* flow() override { return _flow; }
    const ::ayt::net::OnlineFlowCoordinator* flow() const override {
        return _flow;
    }
    OnlineSceneBridgeStatus getBridgeStatus() const override {
        return _bridge ? _bridge->getStatus() : OnlineSceneBridgeStatus{};
    }
    bool retryMainMenuScene() override {
        return _bridge && _bridge->retryMainMenuScene();
    }

private:
    ::ayt::app::IEngineHost& _host;
    ::ayt::net::IOnlineFlowSubSystem& _flowSubSystem;
    OnlineSceneBridgeConfig _config;
    ::ayt::net::OnlineFlowCoordinator* _flow = nullptr;
    std::unique_ptr<OnlineSceneBridge> _bridge;
};

} // namespace

std::unique_ptr<IOnlineApplicationSubSystem>
createOnlineApplicationSubSystem(
    ::ayt::app::IEngineHost& host,
    ::ayt::net::IOnlineFlowSubSystem& flow,
    OnlineSceneBridgeConfig config) {
    return std::make_unique<OnlineApplicationSubSystem>(
        host, flow, std::move(config));
}

IOnlineApplicationSubSystem* findRegisteredOnlineApplicationSubSystem() {
    auto* system = ::ayt::game::SubSystemRegistry::instance().findSubSystem(
        "OnlineApplication");
    return dynamic_cast<IOnlineApplicationSubSystem*>(system);
}

bool registerOnlineApplication(
    ::ayt::app::IEngineHost& host,
    OnlineApplicationConfig config,
    ::ayt::net::OnlineSubSystemDependencies dependencies) {
    if (findRegisteredOnlineApplicationSubSystem()) return true;
    if (!config.isValid()) return false;
    if (!::ayt::net::registerOnlineSubSystem(
            config.online, std::move(dependencies), &host.eventBus())) {
        return false;
    }
    if (!::ayt::net::registerOnlineFlowSubSystem(
            config.flow, &host.eventBus())) {
        return false;
    }
    auto* flow = ::ayt::net::findRegisteredOnlineFlowSubSystem();
    if (!flow) return false;
    auto system = createOnlineApplicationSubSystem(
        host, *flow, std::move(config.scenes));
    ::ayt::game::IGameLoop::instance().registerSubSystem(system.release());
    return findRegisteredOnlineApplicationSubSystem() != nullptr;
}

bool registerOnlineApplication(
    OnlineApplicationConfig config,
    ::ayt::net::OnlineSubSystemDependencies dependencies) {
    return registerOnlineApplication(
        ::ayt::app::defaultEngineHost(),
        std::move(config), std::move(dependencies));
}

} // namespace ayt::app::online

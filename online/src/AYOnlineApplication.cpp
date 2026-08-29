#include <AYOnlineApplication/OnlineApplication.h>

#include <AYEventSystem/EventBus.h>
#include <AYGameLoop/SubSystemRegistry.h>

#include <algorithm>
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

bool OnlineContentMapping::isValid() const {
    return !contentId.empty() && contentId.size() <= 128 &&
           !contentVersion.empty() && contentVersion.size() <= 64 &&
           !scenePath.empty() && scenePath.size() <= kMaximumScenePathLength &&
           sceneName.size() <= kMaximumSceneNameLength;
}

bool OnlineContentCatalog::addOrReplace(OnlineContentMapping mapping) {
    if (!mapping.isValid()) return false;
    const auto found = std::find_if(
        _mappings.begin(), _mappings.end(), [&](const auto& current) {
            return current.contentId == mapping.contentId &&
                   current.contentVersion == mapping.contentVersion;
        });
    if (found == _mappings.end()) {
        _mappings.push_back(std::move(mapping));
    } else {
        *found = std::move(mapping);
    }
    return true;
}

OnlineContentResolveResult OnlineContentCatalog::resolve(
    const ::ayt::net::OnlineContentDescriptor& content) const {
    if (!content.isValid()) {
        return {.message = "Online content descriptor is invalid"};
    }
    const auto found = std::find_if(
        _mappings.begin(), _mappings.end(), [&](const auto& mapping) {
            return mapping.contentId == content.contentId &&
                   mapping.contentVersion == content.contentVersion;
        });
    if (found == _mappings.end()) {
        return {.message = "Content is not installed: " + content.contentId +
                           "@" + content.contentVersion};
    }
    return {
        .scenePath = found->scenePath,
        .sceneName = found->sceneName,
    };
}

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
        if (loadConnection != 0) eventBus.unsubscribe(loadConnection);
        if (statusConnection != 0) eventBus.unsubscribe(statusConnection);
        if (sceneConnection != 0) eventBus.unsubscribe(sceneConnection);
        loadConnection = 0;
        statusConnection = 0;
        sceneConnection = 0;
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
        if (isMenuState(event.state) && status.sessionSceneActive &&
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
            status.sessionSceneActive = true;
            status.mainMenuRecoveryRequired = false;
            status.activeContent = content;
            const auto currentFlow = flow.getStatus();
            if (currentFlow.state == ::ayt::net::OnlineFlowState::LoadingSession &&
                currentFlow.loadingGeneration == flowGeneration) {
                (void)flow.completeLoading(flowGeneration);
            }
        } else {
            status.sessionSceneActive = false;
            status.mainMenuRecoveryRequired = false;
            status.activeContent = {};
        }
    }

    ::ayt::net::OnlineFlowCoordinator& flow;
    ::ayt::app::IRuntimeSceneLoader& loader;
    OnlineSceneBridgeConfig config;
    ::ayt::event::EventBus& eventBus;
    ::ayt::event::ConnectionId loadConnection = 0;
    ::ayt::event::ConnectionId statusConnection = 0;
    ::ayt::event::ConnectionId sceneConnection = 0;
    uint64_t nextSceneRequestId = 0;
    PendingPurpose pendingPurpose = PendingPurpose::None;
    ::ayt::net::OnlineContentDescriptor pendingContent;
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
                               OnlineSceneBridgeConfig config)
        : _host(host), _config(std::move(config)) {}

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
        auto* flowSubSystem = ::ayt::net::findRegisteredOnlineFlowSubSystem();
        auto* loader = _host.service<::ayt::app::IRuntimeSceneLoader>(
            ::ayt::app::kHostServiceRuntimeSceneLoader);
        if (!flowSubSystem || !flowSubSystem->isReady() ||
            !flowSubSystem->coordinator() || !loader) return false;
        _flow = flowSubSystem->coordinator();
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
    OnlineSceneBridgeConfig _config;
    ::ayt::net::OnlineFlowCoordinator* _flow = nullptr;
    std::unique_ptr<OnlineSceneBridge> _bridge;
};

} // namespace

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
    auto system = std::make_unique<OnlineApplicationSubSystem>(
        host, std::move(config.scenes));
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

#include <AYOnlineApplication/OnlineApplication.h>

#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
#include <AYTest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace online = ayt::app::online;

namespace
{

class FakeOnlineSubSystem final : public ayt::net::IOnlineSubSystem {
public:
    const char* getName() const override { return "FakeOnline"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override {
        static ayt::game::SubSystemDescriptor descriptor{};
        return descriptor;
    }
    bool initialize() override { return true; }
    void update(float) override {}
    void fixedUpdate(float) override {}
    void shutdown() override {}
    bool isReady() const override { return true; }
    bool isAuthenticated() const override { return authenticated; }
    ayt::net::PeerId getLocalPeerId() const override {
        return ayt::net::PeerId("local");
    }
    bool setPlayerAccessToken(std::string token) override {
        authenticated = !token.empty();
        return true;
    }
    bool setP2PAdmissionToken(std::string) override { return true; }
    bool listLobbies(ayt::net::ListLobbiesRequest) override { return true; }
    bool createLobby(ayt::net::CreateLobbyRequest) override { return true; }
    bool joinLobby(ayt::net::LobbyId) override { return true; }
    bool refreshLobby() override { return true; }
    bool updateLobbyName(std::string) override { return true; }
    bool leaveLobby() override { settleIdle(); return true; }
    bool launchLobbyP2P(uint16_t) override { return true; }
    bool startMatchmaking(ayt::net::MatchmakingRequest) override { return true; }
    bool cancelMatchmaking() override { settleIdle(); return true; }
    bool leaveSession() override {
        ++leaveSessionCalls;
        settleIdle();
        return true;
    }
    bool reset() override { settleIdle(); return true; }
    ayt::net::OnlineSessionCoordinatorStatus getOnlineStatus() const override {
        return onlineStatus;
    }
    ayt::net::P2PSessionCoordinatorStatus getP2PStatus() const override {
        return p2pStatus;
    }
    std::vector<ayt::net::LobbyInfo> getLobbyResults() const override {
        return {};
    }
    uint64_t getLobbyListGeneration() const override { return 0; }

    void settleIdle() {
        onlineStatus.state = ayt::net::OnlineSessionCoordinatorState::Idle;
        onlineStatus.topology = ayt::net::OnlineSessionTopology::None;
        onlineStatus.assignment = {};
        p2pStatus = {};
    }

    void setP2PSession(bool connected, uint64_t sessionId,
                       uint32_t epoch) {
        onlineStatus.state = connected
            ? ayt::net::OnlineSessionCoordinatorState::InSession
            : ayt::net::OnlineSessionCoordinatorState::Connecting;
        onlineStatus.topology = ayt::net::OnlineSessionTopology::P2P;
        onlineStatus.assignment.topology = ayt::net::MatchTopology::P2P;
        onlineStatus.assignment.content = {
            "maps/arena", "content-1", 42};
        p2pStatus.backendSession.sessionId = sessionId;
        p2pStatus.backendSession.epoch = epoch;
        p2pStatus.networkSession.sessionId = sessionId;
        p2pStatus.networkSession.epoch = epoch;
        p2pStatus.state = connected
            ? ayt::net::P2PSessionCoordinatorState::Active
            : ayt::net::P2PSessionCoordinatorState::Connecting;
    }

    bool authenticated = false;
    ayt::net::OnlineSessionCoordinatorStatus onlineStatus;
    ayt::net::P2PSessionCoordinatorStatus p2pStatus;
    uint32_t leaveSessionCalls = 0;
};

std::filesystem::path writeEmptyScene(const char* fileName) {
    const auto path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream output(path, std::ios::trunc);
    output << "{}";
    return path;
}

class ActiveBridgeHarness {
public:
    explicit ActiveBridgeHarness(std::string suffix)
        : flow(fakeOnline, {}, &bus), _suffix(std::move(suffix)) {}

    void initialize(
        const std::function<void(online::OnlineSceneBridgeConfig&)>& configure = {}) {
        sessionPath = writeEmptyScene(
            ("ay_online_application_" + _suffix + "_session.ayscene").c_str());
        menuPath = writeEmptyScene(
            ("ay_online_application_" + _suffix + "_menu.ayscene").c_str());
        scenes = ayt::app::defaultEngineHost().scenes();
        scenes->setCurrent(nullptr);
        loader = ayt::app::createRuntimeSceneLoader(*scenes, {}, &bus);
        CHECK(loader->initialize());

        catalog = std::make_shared<online::OnlineContentCatalog>();
        CHECK(catalog->addOrReplace({
            "maps/arena", "content-1", sessionPath.string(), "Arena"}));
        online::OnlineSceneBridgeConfig config;
        config.contentResolver = catalog;
        config.mainMenuScenePath = menuPath.string();
        config.mainMenuSceneName = "MainMenu";
        if (configure) configure(config);
        bridge = std::make_unique<online::OnlineSceneBridge>(
            flow, *loader, std::move(config), bus);
        CHECK(bridge->initialize());
    }

    void activate(uint64_t sessionId = 700, uint32_t epoch = 3) {
        CHECK(flow.signIn("player-token"));
        bus.pump();
        fakeOnline.setP2PSession(false, sessionId, epoch);
        flow.update();
        bus.pump();
        CHECK(bridge->getStatus().loadPending);
        loader->update(0.0f);
        fakeOnline.setP2PSession(true, sessionId, epoch);
        bus.pump();
        CHECK(flow.getStatus().state == ayt::net::OnlineFlowState::InSession);
        CHECK(bridge->getStatus().sessionSceneActive);
    }

    ~ActiveBridgeHarness() {
        if (bridge) bridge->shutdown();
        if (loader) loader->shutdown();
        if (!sessionPath.empty()) std::filesystem::remove(sessionPath);
        if (!menuPath.empty()) std::filesystem::remove(menuPath);
    }

    ayt::event::EventBus bus;
    FakeOnlineSubSystem fakeOnline;
    ayt::net::OnlineFlowCoordinator flow;
    ayt::scene::SceneManager* scenes = nullptr;
    std::unique_ptr<ayt::app::IRuntimeSceneLoader> loader;
    std::filesystem::path sessionPath;
    std::filesystem::path menuPath;
    std::shared_ptr<online::OnlineContentCatalog> catalog;
    std::unique_ptr<online::OnlineSceneBridge> bridge;

private:
    std::string _suffix;
};

} // namespace

TEST_SUITE(OnlineApplicationBridgeTest)

TEST_CASE(catalog_requires_exact_content_version)
{
    online::OnlineContentCatalog catalog;
    CHECK(catalog.addOrReplace({"arena", "v1", "arena.ayscene", "Arena"}));
    CHECK(catalog.resolve({"arena", "v1", 7}).isValid());
    const auto missing = catalog.resolve({"arena", "v2", 7});
    CHECK_FALSE(missing.isValid());
    CHECK_FALSE(missing.message.empty());
}

TEST_CASE(loads_assignment_then_returns_to_local_main_menu)
{
    ayt::event::EventBus bus;
    FakeOnlineSubSystem fakeOnline;
    ayt::net::OnlineFlowCoordinator flow(fakeOnline, {}, &bus);

    auto* scenes = ayt::app::defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    auto loader = ayt::app::createRuntimeSceneLoader(*scenes, {}, &bus);
    CHECK(loader->initialize());

    const auto sessionPath = writeEmptyScene(
        "ay_online_application_session.ayscene");
    const auto menuPath = writeEmptyScene(
        "ay_online_application_menu.ayscene");
    auto catalog = std::make_shared<online::OnlineContentCatalog>();
    CHECK(catalog->addOrReplace({
        "maps/arena", "content-1", sessionPath.string(), "Arena"}));

    uint64_t preparedSeed = 0;
    online::OnlineSceneBridgeConfig config;
    config.contentResolver = catalog;
    config.mainMenuScenePath = menuPath.string();
    config.mainMenuSceneName = "MainMenu";
    config.prepareSessionScene =
        [&](const ayt::net::OnlineContentDescriptor& content,
            ayt::scene::Scene& scene,
            std::string&) {
            preparedSeed = content.contentSeed;
            return scene.name() == "Arena";
        };

    online::OnlineSceneBridge bridge(flow, *loader, config, bus);
    CHECK(bridge.initialize());
    CHECK(flow.signIn("player-token"));
    bus.pump();

    fakeOnline.onlineStatus.state =
        ayt::net::OnlineSessionCoordinatorState::Connecting;
    fakeOnline.onlineStatus.topology = ayt::net::OnlineSessionTopology::P2P;
    fakeOnline.onlineStatus.assignment.topology = ayt::net::MatchTopology::P2P;
    fakeOnline.onlineStatus.assignment.content = {
        "maps/arena", "content-1", 42};
    flow.update();
    bus.pump();

    CHECK(bridge.getStatus().loadPending);
    loader->update(0.0f);
    fakeOnline.onlineStatus.state =
        ayt::net::OnlineSessionCoordinatorState::InSession;
    bus.pump();
    CHECK(preparedSeed == 42);
    CHECK(loader->currentScene()->name() == "Arena");
    CHECK(flow.getStatus().state == ayt::net::OnlineFlowState::InSession);
    CHECK(bridge.getStatus().sessionSceneActive);

    CHECK(flow.leaveSession());
    flow.update();
    bus.pump();
    CHECK(bridge.getStatus().loadPending);
    loader->update(0.0f);
    bus.pump();
    CHECK(loader->currentScene()->name() == "MainMenu");
    CHECK_FALSE(bridge.getStatus().sessionSceneActive);

    bridge.shutdown();
    loader->shutdown();
    std::filesystem::remove(sessionPath);
    std::filesystem::remove(menuPath);
}

TEST_CASE(missing_session_scene_preserves_active_main_menu)
{
    ayt::event::EventBus bus;
    FakeOnlineSubSystem fakeOnline;
    ayt::net::OnlineFlowCoordinator flow(fakeOnline, {}, &bus);

    const auto menuPath = writeEmptyScene(
        "ay_online_application_failure_menu.ayscene");
    const auto missingPath = std::filesystem::temp_directory_path() /
        "ay_online_application_missing_session.ayscene";
    std::filesystem::remove(missingPath);

    auto* scenes = ayt::app::defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    ayt::app::RuntimeSceneLoaderConfig loaderConfig;
    loaderConfig.initialSceneName = "MainMenu";
    loaderConfig.initialScenePath = menuPath.string();
    auto loader = ayt::app::createRuntimeSceneLoader(
        *scenes, std::move(loaderConfig), &bus);
    CHECK(loader->initialize());

    auto catalog = std::make_shared<online::OnlineContentCatalog>();
    CHECK(catalog->addOrReplace({
        "maps/missing", "content-1", missingPath.string(), "Missing"}));
    online::OnlineSceneBridgeConfig config;
    config.contentResolver = catalog;
    config.mainMenuScenePath = menuPath.string();

    online::OnlineSceneBridge bridge(flow, *loader, config, bus);
    CHECK(bridge.initialize());
    CHECK(flow.signIn("player-token"));
    bus.pump();

    fakeOnline.onlineStatus.state =
        ayt::net::OnlineSessionCoordinatorState::Connecting;
    fakeOnline.onlineStatus.topology = ayt::net::OnlineSessionTopology::P2P;
    fakeOnline.onlineStatus.assignment.topology =
        ayt::net::MatchTopology::P2P;
    fakeOnline.onlineStatus.assignment.content = {
        "maps/missing", "content-1", 101};
    flow.update();
    bus.pump();
    CHECK(bridge.getStatus().loadPending);

    loader->update(0.0f);
    bus.pump();
    CHECK(flow.getStatus().state == ayt::net::OnlineFlowState::Leaving);
    flow.update();
    bus.pump();
    CHECK(flow.getStatus().state == ayt::net::OnlineFlowState::Failed);
    CHECK(flow.getStatus().error == ayt::net::OnlineFlowError::LoadingFailed);
    CHECK(loader->currentScene() != nullptr);
    CHECK(loader->currentScene()->name() == "MainMenu");
    CHECK_FALSE(bridge.getStatus().sessionSceneActive);
    CHECK_FALSE(bridge.getStatus().lastError.empty());

    bridge.shutdown();
    loader->shutdown();
    std::filesystem::remove(menuPath);
}

TEST_CASE(loading_timeout_cancels_stale_scene_request)
{
    uint64_t nowMs = 100;
    ayt::event::EventBus bus;
    FakeOnlineSubSystem fakeOnline;
    ayt::net::OnlineFlowConfig flowConfig;
    flowConfig.loadingTimeoutMs = 10;
    flowConfig.nowMilliseconds = [&] { return nowMs; };
    ayt::net::OnlineFlowCoordinator flow(fakeOnline, flowConfig, &bus);

    const auto menuPath = writeEmptyScene(
        "ay_online_application_timeout_menu.ayscene");
    const auto sessionPath = writeEmptyScene(
        "ay_online_application_timeout_session.ayscene");
    auto* scenes = ayt::app::defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    ayt::app::RuntimeSceneLoaderConfig loaderConfig;
    loaderConfig.initialSceneName = "MainMenu";
    loaderConfig.initialScenePath = menuPath.string();
    auto loader = ayt::app::createRuntimeSceneLoader(
        *scenes, std::move(loaderConfig), &bus);
    CHECK(loader->initialize());

    auto catalog = std::make_shared<online::OnlineContentCatalog>();
    CHECK(catalog->addOrReplace({
        "maps/slow", "content-1", sessionPath.string(), "SlowArena"}));
    online::OnlineSceneBridgeConfig config;
    config.contentResolver = catalog;
    config.mainMenuScenePath = menuPath.string();
    online::OnlineSceneBridge bridge(flow, *loader, config, bus);
    CHECK(bridge.initialize());
    CHECK(flow.signIn("player-token"));
    bus.pump();

    fakeOnline.onlineStatus.state =
        ayt::net::OnlineSessionCoordinatorState::Connecting;
    fakeOnline.onlineStatus.topology = ayt::net::OnlineSessionTopology::P2P;
    fakeOnline.onlineStatus.assignment.topology =
        ayt::net::MatchTopology::P2P;
    fakeOnline.onlineStatus.assignment.content = {
        "maps/slow", "content-1", 202};
    flow.update();
    bus.pump();
    CHECK(loader->getLoadStatus().state ==
          ayt::app::RuntimeSceneLoadState::Queued);

    // Do not pump the Egress loader: the online deadline must invalidate the
    // queued generation before it can activate the staged Arena.
    nowMs += 11;
    flow.update();
    bus.pump();
    CHECK(flow.getStatus().state == ayt::net::OnlineFlowState::Leaving);
    flow.update();
    bus.pump();
    CHECK(flow.getStatus().state == ayt::net::OnlineFlowState::Failed);
    CHECK(flow.getStatus().error == ayt::net::OnlineFlowError::LoadingTimedOut);
    CHECK(loader->getLoadStatus().state ==
          ayt::app::RuntimeSceneLoadState::Cancelled);
    CHECK(loader->currentScene() != nullptr);
    CHECK(loader->currentScene()->name() == "MainMenu");
    CHECK_FALSE(bridge.getStatus().loadPending);
    CHECK_FALSE(bridge.getStatus().sessionSceneActive);

    bridge.shutdown();
    loader->shutdown();
    std::filesystem::remove(sessionPath);
    std::filesystem::remove(menuPath);
}

TEST_CASE(reconnect_suspends_and_resumes_scene_once)
{
    uint32_t suspendCalls = 0;
    uint32_t resumeCalls = 0;
    uint64_t resumedGeneration = 0;
    uint32_t resumedEpoch = 0;
    ActiveBridgeHarness harness("reconnect");
    harness.initialize([&](auto& config) {
        config.suspendSessionScene = [&](ayt::scene::Scene&) {
            ++suspendCalls;
        };
        config.resumeSessionScene =
            [&](ayt::scene::Scene&, uint64_t generation, uint32_t epoch,
                std::string&) {
                ++resumeCalls;
                resumedGeneration = generation;
                resumedEpoch = epoch;
                return true;
            };
    });
    harness.activate();

    harness.fakeOnline.setP2PSession(false, 700, 3);
    harness.flow.update();
    harness.bus.pump();
    CHECK(harness.bridge->getStatus().sessionSceneSuspended);
    CHECK_INT_EQ(suspendCalls, 1);
    CHECK(harness.bridge->getStatus().recoveryGeneration == 1);

    harness.fakeOnline.setP2PSession(true, 700, 4);
    harness.flow.update();
    harness.bus.pump();
    CHECK_FALSE(harness.bridge->getStatus().sessionSceneSuspended);
    CHECK_INT_EQ(resumeCalls, 1);
    CHECK(resumedGeneration == 1);
    CHECK(resumedEpoch == 4);
    CHECK(harness.bridge->getStatus().activeSessionEpoch == 4);

    // A delayed event from the prior transport snapshot must not suspend the
    // already recovered world a second time.
    const auto current = harness.flow.getStatus();
    ayt::net::OnlineFlowStatusChangedEvent stale;
    stale.state = current.state;
    stale.sessionState = ayt::net::OnlineSessionCoordinatorState::Connecting;
    stale.loadingGeneration = current.loadingGeneration;
    stale.sessionId = current.sessionId;
    stale.sessionEpoch = current.sessionEpoch;
    stale.worldLoaded = current.worldLoaded;
    harness.bus.post(stale);
    harness.bus.pump();
    CHECK_INT_EQ(suspendCalls, 1);
    CHECK_INT_EQ(resumeCalls, 1);
    CHECK(harness.bridge->getStatus().recoveryGeneration == 1);
}

TEST_CASE(reconnect_callback_failure_returns_to_main_menu)
{
    uint32_t deactivateCalls = 0;
    ActiveBridgeHarness harness("reconnect_failure");
    harness.initialize([&](auto& config) {
        config.resumeSessionScene =
            [](ayt::scene::Scene&, uint64_t, uint32_t, std::string& message) {
                message = "replication bindings could not be restored";
                return false;
            };
        config.deactivateSessionScene = [&](ayt::scene::Scene&) {
            ++deactivateCalls;
        };
    });
    harness.activate();

    harness.fakeOnline.setP2PSession(false, 700, 3);
    harness.flow.update();
    harness.bus.pump();
    harness.fakeOnline.setP2PSession(true, 700, 4);
    harness.flow.update();
    harness.bus.pump();
    CHECK(harness.flow.getStatus().state == ayt::net::OnlineFlowState::Leaving);
    CHECK(harness.flow.getStatus().error == ayt::net::OnlineFlowError::WorldFailed);

    harness.flow.update();
    harness.bus.pump();
    CHECK(harness.flow.getStatus().state == ayt::net::OnlineFlowState::Failed);
    CHECK(harness.flow.getStatus().message ==
          "replication bindings could not be restored");
    CHECK_INT_EQ(deactivateCalls, 1);
    CHECK(harness.bridge->getStatus().loadPending);
    harness.loader->update(0.0f);
    harness.bus.pump();
    CHECK(harness.loader->currentScene()->name() == "MainMenu");
    CHECK_FALSE(harness.bridge->getStatus().sessionSceneActive);
}

TEST_CASE(unexpected_scene_switch_fails_active_session_closed)
{
    uint32_t suspendCalls = 0;
    uint32_t deactivateCalls = 0;
    ActiveBridgeHarness harness("external_switch");
    harness.initialize([&](auto& config) {
        config.suspendSessionScene = [&](ayt::scene::Scene&) {
            ++suspendCalls;
        };
        config.deactivateSessionScene = [&](ayt::scene::Scene&) {
            ++deactivateCalls;
        };
    });
    harness.activate();

    ayt::scene::Scene rogue(ayt::scene::SceneMode::Play, "Rogue");
    harness.scenes->setCurrent(&rogue);
    harness.bus.post(ayt::event::SceneCurrentChangedEvent{&rogue});
    harness.bus.pump();
    CHECK(harness.flow.getStatus().state == ayt::net::OnlineFlowState::Leaving);
    CHECK(harness.flow.getStatus().error == ayt::net::OnlineFlowError::WorldFailed);
    CHECK_INT_EQ(suspendCalls, 1);
    CHECK_INT_EQ(deactivateCalls, 1);

    harness.flow.update();
    harness.bus.pump();
    harness.loader->update(0.0f);
    harness.bus.pump();
    CHECK(harness.flow.getStatus().state == ayt::net::OnlineFlowState::Failed);
    CHECK(harness.loader->currentScene()->name() == "MainMenu");
    CHECK_FALSE(harness.bridge->getStatus().sessionSceneActive);
}

TEST_SUITE_END

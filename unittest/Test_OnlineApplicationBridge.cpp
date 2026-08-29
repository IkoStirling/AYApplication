#include <AYOnlineApplication/OnlineApplication.h>

#include <AYEventSystem/EventBus.h>
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
    bool leaveSession() override { settleIdle(); return true; }
    bool reset() override { settleIdle(); return true; }
    ayt::net::OnlineSessionCoordinatorStatus getOnlineStatus() const override {
        return onlineStatus;
    }
    ayt::net::P2PSessionCoordinatorStatus getP2PStatus() const override {
        return {};
    }
    std::vector<ayt::net::LobbyInfo> getLobbyResults() const override {
        return {};
    }
    uint64_t getLobbyListGeneration() const override { return 0; }

    void settleIdle() {
        onlineStatus.state = ayt::net::OnlineSessionCoordinatorState::Idle;
        onlineStatus.topology = ayt::net::OnlineSessionTopology::None;
        onlineStatus.assignment = {};
    }

    bool authenticated = false;
    ayt::net::OnlineSessionCoordinatorStatus onlineStatus;
};

std::filesystem::path writeEmptyScene(const char* fileName) {
    const auto path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream output(path, std::ios::trunc);
    output << "{}";
    return path;
}

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

TEST_SUITE_END

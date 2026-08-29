// Runnable first-integration reference:
// MainMenu -> Lobby -> Arena -> reflected ghost replication -> MainMenu.

#include <AYOnlineApplication/OnlineApplication.h>

#include <AYApplication/IEngineHost.h>
#include <AYEventSystem/EventBus.h>
#include <AYNetwork/Protocol/PacketCodec.h>
#include <AYNetwork/Replication/ReflectSerializer.h>
#include <AYNetwork/Session/InMemoryOnlineServices.h>
#include <AYNetwork/Session/InMemorySessionService.h>
#include <AYReflect/detail/ReflectImpl.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
#include <AYTest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ayt::net;
namespace online = ayt::app::online;

namespace
{

struct ArenaPlayerGhost {
    int32_t score = 0;
    uint64_t contentSeed = 0;
};

struct ArenaPlayerGhostRegistrar {
    ArenaPlayerGhostRegistrar() {
        auto& registry = ayt::reflect::TypeRegistryImpl::instance();
        if (registry.findType("OnlineVerticalSliceGhost")) return;
        using Info = ayt::reflect::TypeInfoImpl<ArenaPlayerGhost>;
        auto* info = new Info(
            "OnlineVerticalSliceGhost",
            ayt::reflect::detail::defaultCreate<ArenaPlayerGhost>,
            ayt::reflect::detail::defaultDestroy<ArenaPlayerGhost>,
            ayt::reflect::detail::defaultCopy<ArenaPlayerGhost>);
        const auto replicated =
            ayt::reflect::FieldAttribute::Serialize |
            ayt::reflect::FieldAttribute::NetReplicate;
        info->addField(new ayt::reflect::FieldInfoImpl(
            "score", registry.findType<int32_t>(),
            offsetof(ArenaPlayerGhost, score), replicated));
        info->addField(new ayt::reflect::FieldInfoImpl(
            "contentSeed", registry.findType<uint64_t>(),
            offsetof(ArenaPlayerGhost, contentSeed), replicated));
        registry.registerTypeInfo("OnlineVerticalSliceGhost", info);
    }
};

ArenaPlayerGhostRegistrar g_arenaPlayerGhostRegistrar;

// The control plane is real; only the GNS socket is replaced by a deterministic
// P2P facade so this reference can run without a public signaling server.
class VerticalSliceNetwork final : public INetworkSubSystem {
public:
    VerticalSliceNetwork() : _replication(this) {}

    const char* getName() const override { return "VerticalSliceNetwork"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override {
        static const ayt::game::SubSystemDescriptor descriptor{
            "VerticalSliceNetwork", {}, 0};
        return descriptor;
    }

    bool initialize() override {
        _initialized = true;
        _session.state = P2PSessionState::Unconfigured;
        return true;
    }
    void update(float) override {}
    void fixedUpdate(float) override {}
    void shutdown() override {
        disconnect();
        _initialized = false;
        _configured = false;
        _signaling.reset();
    }

    void connect(const char*, uint16_t) override {}
    void listen(uint16_t) override {}
    void disconnect() override {
        _connected = false;
        _listening = false;
        _session.role = P2PSessionRole::None;
        _session.state = _configured
            ? P2PSessionState::Idle : P2PSessionState::Unconfigured;
    }
    bool isConnected() const override { return _connected; }
    bool isListening() const override { return _listening; }
    ConnectionMode getMode() const override {
        if (_listening) return ConnectionMode::ListenServer;
        if (_connected) return ConnectionMode::Client;
        return ConnectionMode::Disconnected;
    }

    bool configureP2P(const P2PConfig& config,
                      std::shared_ptr<ISignalingTransport> signaling) override {
        if (!_initialized || !config.isValid() || !signaling) return false;
        _config = config;
        _signaling = std::move(signaling);
        _configured = true;
        _session = {};
        _session.state = P2PSessionState::Idle;
        _session.localPeerId = _config.localPeerId;
        _session.virtualPort = _config.virtualPort;
        _session.sessionId = _config.sessionId;
        _session.epoch = _config.sessionEpoch;
        _session.migration = _migrationEnabled
            ? P2PHostMigrationState::Stable
            : P2PHostMigrationState::Disabled;
        return true;
    }
    bool listenP2P() override {
        if (!_configured) return false;
        _listening = true;
        _session.role = P2PSessionRole::Host;
        _session.state = P2PSessionState::Hosting;
        _session.hostPeerId = _config.localPeerId;
        _session.localSeatId = 1;
        return true;
    }
    bool connectP2P(const PeerId& remotePeer) override {
        if (!_configured || !remotePeer.isValid()) return false;
        _connected = true;
        _session.role = P2PSessionRole::Client;
        _session.state = P2PSessionState::Connecting;
        _session.hostPeerId = remotePeer;
        _session.localSeatId = 2;
        return true;
    }
    bool isP2PConfigured() const override { return _configured; }
    PeerId getLocalPeerId() const override { return _config.localPeerId; }
    P2PSessionInfo getP2PSessionInfo() const override { return _session; }
    bool setP2PJoinTicket(const void* data, size_t size) override {
        if (!data && size == 0) {
            _joinTicket.clear();
            return true;
        }
        if (!data || size == 0 || size > kP2PMaxJoinTicketBytes) return false;
        const auto* bytes = static_cast<const uint8_t*>(data);
        _joinTicket.assign(bytes, bytes + size);
        return true;
    }
    void setP2PSessionJoinValidator(
        P2PSessionJoinValidator validator) override {
        _joinValidator = std::move(validator);
    }
    void setP2PHostMigrationEnabled(bool enabled) override {
        _migrationEnabled = enabled;
        _session.migration = enabled ? P2PHostMigrationState::Stable
                                     : P2PHostMigrationState::Disabled;
    }
    void setP2PAuthorityTransitionGate(
        P2PAuthorityTransitionGate gate) override {
        _authorityGate = std::move(gate);
    }
    bool setP2PAuthorityTransitionTimeoutMs(uint32_t timeoutMs) override {
        _authorityTimeoutMs = timeoutMs;
        return timeoutMs != 0 && timeoutMs <= 60000;
    }

    void send(uint8_t, const void*, size_t) override {}
    void sendTo(NetConnection*, uint8_t, const void*, size_t) override {}
    void broadcast(uint8_t, const void*, size_t) override {}
    void broadcastExcept(NetConnection*, uint8_t, const void*, size_t) override {}
    void onMessage(uint8_t, MessageHandler) override {}
    void onConnectionChange(ConnectionHandler) override {}
    void setAcceptCallback(AcceptCallback) override {}
    void kickConnection(NetConnection*, const char*) override {}
    const std::vector<NetConnection*>& getConnections() override {
        return _connections;
    }
    NetConnection* getConnection() const override { return nullptr; }
    uint32_t getHostId() const override { return 0; }
    void setExtension(INetworkExtension* extension) override {
        _replication.setExtension(extension);
    }
    ReplicationManager* getReplicationManager() override {
        return &_replication;
    }
    RpcHandler* getRpcHandler() override { return nullptr; }
    void setReplayRecorder(ayt::replay::IReplayRecorder* recorder) override {
        _replication.setReplayRecorder(recorder);
    }
    ayt::replay::IReplayRecorder* getReplayRecorder() const override {
        return _replication.getReplayRecorder();
    }

private:
    bool _initialized = false;
    bool _configured = false;
    bool _connected = false;
    bool _listening = false;
    bool _migrationEnabled = false;
    uint32_t _authorityTimeoutMs = 0;
    P2PConfig _config;
    P2PSessionInfo _session;
    std::vector<uint8_t> _joinTicket;
    P2PSessionJoinValidator _joinValidator;
    P2PAuthorityTransitionGate _authorityGate;
    std::shared_ptr<ISignalingTransport> _signaling;
    std::vector<NetConnection*> _connections;
    ReplicationManager _replication;
};

std::filesystem::path writeScene(const char* fileName) {
    const auto path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream output(path, std::ios::trunc);
    output << "{}";
    return path;
}

std::shared_ptr<InMemoryP2PSessionService> makeSessionService() {
    InMemoryP2PSessionServiceConfig config;
    config.publicSignalingAddress = "127.0.0.1";
    config.signalingPort = 28080;
    config.nowUnixSeconds = [] { return uint64_t{1000}; };
    return std::make_shared<InMemoryP2PSessionService>(std::move(config));
}

OnlineSubSystemConfig makeOnlineConfig() {
    OnlineSubSystemConfig config;
    config.localPeerId = PeerId{"vertical-host"};
    config.p2p.p2p.localPeerId = config.localPeerId;
    config.p2p.p2p.virtualPort = 7350;
    config.p2p.p2p.icePolicy = P2PIcePolicy::DirectOnly;
    config.p2p.p2p.allowPrivateCandidates = true;
    config.p2p.capacity = 2;
    config.p2p.authorityRetryMs = 5;
    config.p2p.authorityTimeoutMs = 1000;
    config.p2p.lease.heartbeatIntervalMs = 50;
    config.p2p.lease.transientFailureRetryMs = 10;
    config.sessions.localPeerId = config.localPeerId;
    config.sessions.matchmakingPollIntervalMs = 1;
    config.gracefulShutdownTimeoutMs = 500;
    return config;
}

template <typename Predicate>
bool pumpUntil(IOnlineSubSystem& onlineSystem,
               OnlineFlowCoordinator& flow,
               online::OnlineSceneBridge& bridge,
               ayt::app::IRuntimeSceneLoader& loader,
               ayt::event::EventBus& bus,
               Predicate&& predicate,
               uint32_t timeoutMs = 2500) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        onlineSystem.update(0.0f);
        flow.update();
        bus.pump();
        bridge.update();
        loader.update(0.0f);
        bus.pump();
        if (predicate()) return true;
        if (flow.getStatus().state == OnlineFlowState::Failed) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

TEST_SUITE(OnlineVerticalSlice)

TEST_CASE(MainMenuLobbyArenaReplicationAndReturn)
{
    ayt::test::setCurrentCase("MainMenuLobbyArenaReplicationAndReturn");

    ayt::event::EventBus bus;
    VerticalSliceNetwork network;
    CHECK(network.initialize());

    auto sessionService = makeSessionService();
    auto onlineServices = std::make_shared<InMemoryOnlineServices>(
        InMemoryOnlineServicesConfig{}, sessionService);
    OnlineSubSystemDependencies dependencies;
    dependencies.sessionService = sessionService;
    dependencies.lobbyService = onlineServices;
    dependencies.matchmakingService = onlineServices;
    auto onlineSystem = createOnlineSubSystem(
        network, makeOnlineConfig(), std::move(dependencies), &bus);
    CHECK(onlineSystem != nullptr);
    if (!onlineSystem) return;
    CHECK(onlineSystem->initialize());

    OnlineFlowConfig flowConfig;
    flowConfig.loadingTimeoutMs = 1000;
    OnlineFlowCoordinator flow(*onlineSystem, flowConfig, &bus);

    const auto menuPath = writeScene("ay_vertical_slice_main_menu.ayscene");
    const auto arenaPath = writeScene("ay_vertical_slice_arena.ayscene");
    auto* scenes = ayt::app::defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    ayt::app::RuntimeSceneLoaderConfig loaderConfig;
    loaderConfig.initialSceneName = "MainMenu";
    loaderConfig.initialScenePath = menuPath.string();
    auto loader = ayt::app::createRuntimeSceneLoader(
        *scenes, std::move(loaderConfig), &bus);
    CHECK(loader->initialize());
    CHECK(loader->currentScene() != nullptr);
    CHECK(loader->currentScene()->name() == "MainMenu");

    auto catalog = std::make_shared<online::OnlineContentCatalog>();
    CHECK(catalog->addOrReplace({
        "maps/arena", "content-1", arenaPath.string(), "Arena"}));
    uint64_t preparedSeed = 0;
    online::OnlineSceneBridgeConfig bridgeConfig;
    bridgeConfig.contentResolver = catalog;
    bridgeConfig.mainMenuScenePath = menuPath.string();
    bridgeConfig.mainMenuSceneName = "MainMenu";
    bridgeConfig.prepareSessionScene =
        [&](const OnlineContentDescriptor& content,
            ayt::scene::Scene& scene,
            std::string&) {
            preparedSeed = content.contentSeed;
            return scene.name() == "Arena";
        };
    online::OnlineSceneBridge bridge(
        flow, *loader, std::move(bridgeConfig), bus);
    CHECK(bridge.initialize());

    // Injected platform/in-memory services are trusted adapters and therefore
    // start authenticated; HTTP-backed clients exercise the explicit sign-in
    // credential path in the coordinator tests.
    CHECK(onlineSystem->isAuthenticated());
    CHECK(flow.getStatus().state == OnlineFlowState::MainMenu);

    CreateLobbyRequest create;
    create.ownerPeerId = PeerId{"ignored-client-value"};
    create.name = "Vertical Slice Lobby";
    create.region = "local";
    create.buildId = "build-1";
    create.content = {"maps/arena", "content-1", 0xA11A7u};
    create.capacity = 2;
    CHECK(flow.createLobby(create));
    CHECK(pumpUntil(*onlineSystem, flow, bridge, *loader, bus, [&] {
        return flow.getStatus().state == OnlineFlowState::InLobby;
    }));

    const LobbyId lobbyId = flow.getStatus().lobbyId;
    CHECK(lobbyId != 0);
    const auto guest = onlineServices->joinLobby(
        lobbyId, PeerId{"vertical-guest"});
    CHECK(guest);
    CHECK(flow.refreshLobby());
    CHECK(pumpUntil(*onlineSystem, flow, bridge, *loader, bus, [&] {
        return flow.getStatus().state == OnlineFlowState::InLobby &&
               onlineSystem->getOnlineStatus().lobby.members.size() == 2;
    }));

    CHECK(flow.startLobbySession(7350));
    CHECK(pumpUntil(*onlineSystem, flow, bridge, *loader, bus, [&] {
        return flow.getStatus().state == OnlineFlowState::InSession;
    }));
    CHECK(flow.getStatus().content == create.content);
    CHECK(preparedSeed == create.content.contentSeed);
    CHECK(loader->currentScene() != nullptr);
    CHECK(loader->currentScene()->name() == "Arena");
    CHECK(bridge.getStatus().sessionSceneActive);

    // Runtime-spawned game state uses the same reflected replication path as
    // a packaged host/client.  The sink below replaces only transport I/O.
    auto* authority = network.getReplicationManager();
    CHECK(authority != nullptr);
    ReplicationManager client(nullptr);
    client.setModeForTesting(ConnectionMode::Client);
    size_t deliveredFrames = 0;
    authority->setBroadcastSinkForTesting(
        [&](uint8_t, const void* data, size_t size) {
            auto packet = PacketCodec::decode(
                static_cast<const uint8_t*>(data), size);
            if (!packet.ok) return;
            BitStream stream(packet.body.data(), packet.body.size());
            if (client.onReceive(packet.header.msgType, stream, nullptr)) {
                ++deliveredFrames;
            }
        });

    auto* ghostType = ayt::reflect::TypeRegistryImpl::instance().findType(
        "OnlineVerticalSliceGhost");
    CHECK(ghostType != nullptr);
    ArenaPlayerGhost hostGhost{10, preparedSeed};
    ArenaPlayerGhost clientGhost{};
    constexpr uint32_t kGhostNetId = 7001;
    authority->registerObject(&hostGhost, ghostType, kGhostNetId);
    authority->tick(0.034f);
    uint64_t schemaHash = 0;
    CHECK(client.peekSpawnAnnouncement(kGhostNetId, schemaHash));
    CHECK(schemaHash == ReflectSerializer::hashTypeSchema(ghostType));
    client.registerObject(&clientGhost, ghostType, kGhostNetId);
    authority->forceReplicate(kGhostNetId);
    authority->tick(0.034f);
    CHECK_INT_EQ(clientGhost.score, 10);
    CHECK(clientGhost.contentSeed == preparedSeed);

    hostGhost.score = 88;
    authority->tick(0.034f);
    CHECK_INT_EQ(clientGhost.score, 88);
    CHECK(deliveredFrames >= 3);

    authority->unregisterObject(kGhostNetId);
    client.unregisterObject(kGhostNetId);
    authority->setBroadcastSinkForTesting(nullptr);

    CHECK(flow.leaveSession());
    CHECK(pumpUntil(*onlineSystem, flow, bridge, *loader, bus, [&] {
        return flow.getStatus().state == OnlineFlowState::MainMenu &&
               loader->currentScene() != nullptr &&
               loader->currentScene()->name() == "MainMenu";
    }));
    CHECK_FALSE(bridge.getStatus().sessionSceneActive);

    (void)onlineServices->leaveLobby(lobbyId, PeerId{"vertical-guest"});
    bridge.shutdown();
    loader->shutdown();
    onlineSystem->shutdown();
    network.shutdown();
    std::filesystem::remove(arenaPath);
    std::filesystem::remove(menuPath);
}

TEST_SUITE_END

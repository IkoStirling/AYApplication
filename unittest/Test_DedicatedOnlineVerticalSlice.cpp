#include <AYOnlineApplication/DedicatedApplication.h>

#include <AYNetwork/Session/InMemoryOnlineServices.h>
#include <AYNetwork/Transport/UdpSocket.h>
#include <AYScene.h>
#include <AYTest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace ayt::net;
namespace online = ayt::app::online;

namespace
{

std::filesystem::path uniqueTempPath(const char* stem,
                                     const char* extension) {
    static std::atomic<uint64_t> counter{0};
    const auto value = ++counter;
    return std::filesystem::temp_directory_path() /
        (std::string{stem} + "_" + std::to_string(value) + extension);
}

std::filesystem::path writeEmptyScene(const char* stem) {
    const auto path = uniqueTempPath(stem, ".ayscene");
    std::ofstream output(path, std::ios::trunc);
    output << "{}";
    return path;
}

uint16_t reserveUdpPort() {
    UdpSocket socket;
    if (!socket.create() || !socket.bind("127.0.0.1", 0)) return 0;
    return socket.getBoundPort();
}

DedicatedAllocation makeAllocation(const std::filesystem::path&,
                                   uint16_t port = 7001) {
    DedicatedAllocation allocation;
    allocation.allocationId = 41;
    allocation.serverId = 7;
    allocation.address = "127.0.0.1";
    allocation.port = port;
    allocation.playerCount = 2;
    allocation.players = {PeerId{"scene-player-a"},
                          PeerId{"scene-player-b"}};
    allocation.matchId = 99;
    allocation.content = {"maps/dedicated-arena", "content-1", 0xA11A7u};
    allocation.reservationToken = std::string(64, 'a');
    allocation.expiresAtUnixSeconds = 9999999999ull;
    return allocation;
}

template <typename Predicate>
bool pumpUntil(DedicatedServerRuntime& runtime,
               NetworkDedicatedSessionConnector* first,
               NetworkDedicatedSessionConnector* second,
               Predicate&& predicate,
               uint32_t timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        runtime.update(0.001f);
        if (first) first->update();
        if (second) second->update();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

TEST_SUITE(DedicatedOnlineVerticalSlice)

TEST_CASE(CatalogResolvesTrustedRelativeSceneAndHostOwnsLifecycle)
{
    ayt::test::setCurrentCase(
        "CatalogResolvesTrustedRelativeSceneAndHostOwnsLifecycle");
    const auto scenePath = writeEmptyScene("ay_dedicated_catalog_scene");
    const auto catalogPath = uniqueTempPath("ay_dedicated_catalog", ".tsv");
    {
        std::ofstream catalogFile(catalogPath, std::ios::trunc);
        catalogFile << "# content catalog\n";
        catalogFile << "maps/dedicated-arena\tcontent-1\t"
                    << scenePath.filename().string() << "\tArenaAuthority\n";
    }

    auto catalog = std::make_shared<online::OnlineContentCatalog>();
    std::string message;
    CHECK(online::loadOnlineContentCatalog(
        catalogPath.string(), *catalog, message));
    CHECK(message.empty());

    uint64_t preparedSeed = 0;
    uint32_t connected = 0;
    uint32_t disconnected = 0;
    uint32_t deactivated = 0;
    online::DedicatedSceneHostConfig config;
    config.contentResolver = catalog;
    config.maximumWorlds = 1;
    config.prepareWorld = [&](const DedicatedAllocation& allocation,
                              ayt::scene::Scene& scene,
                              std::string&) {
        preparedSeed = allocation.content.contentSeed;
        return scene.mode() == ayt::scene::SceneMode::Play &&
               scene.name() == "ArenaAuthority";
    };
    config.onPlayerConnected = [&](const DedicatedAllocation&,
                                   ayt::scene::Scene&,
                                   const PeerId&, NetConnection*) {
        ++connected;
    };
    config.onPlayerDisconnected = [&](const DedicatedAllocation&,
                                      ayt::scene::Scene&,
                                      const PeerId&) {
        ++disconnected;
    };
    config.deactivateWorld = [&](const DedicatedAllocation&,
                                 ayt::scene::Scene&) {
        ++deactivated;
    };
    online::DedicatedSceneHost worlds(std::move(config));
    const auto allocation = makeAllocation(scenePath);
    CHECK(worlds.startAuthoritativeWorld(allocation));
    CHECK_FALSE(worlds.startAuthoritativeWorld(allocation));
    CHECK(worlds.getStatus().lastError ==
          online::DedicatedSceneHostError::DuplicateAllocation);

    auto* scene = worlds.findScene(allocation.allocationId);
    CHECK(scene != nullptr);
    if (scene) {
        CHECK(scene->path() == scenePath.string());
        CHECK(scene->name() == "ArenaAuthority");
        worlds.tickAuthoritativeWorlds(1.0f);
        CHECK_INT_EQ(scene->tickCount(), 1);
    }
    CHECK(preparedSeed == allocation.content.contentSeed);

    worlds.playerConnected(
        allocation.allocationId, PeerId{"scene-player-a"}, nullptr);
    worlds.playerConnected(
        allocation.allocationId, PeerId{"not-allocated"}, nullptr);
    CHECK_INT_EQ(connected, 1);
    CHECK_INT_EQ(worlds.getStatus().connectedPlayers, 1);
    worlds.stopAuthoritativeWorld(allocation.allocationId);
    CHECK(worlds.findScene(allocation.allocationId) == nullptr);
    CHECK_INT_EQ(disconnected, 1);
    CHECK_INT_EQ(deactivated, 1);
    CHECK_INT_EQ(worlds.getStatus().activeWorlds, 0);

    std::filesystem::remove(catalogPath);
    std::filesystem::remove(scenePath);
}

TEST_CASE(SceneBackedRuntimeAdmitsTwoClientsAndReleasesAfterLastLeave)
{
    ayt::test::setCurrentCase(
        "SceneBackedRuntimeAdmitsTwoClientsAndReleasesAfterLastLeave");
    const auto scenePath = writeEmptyScene("ay_dedicated_runtime_scene");
    auto catalog = std::make_shared<online::OnlineContentCatalog>();
    CHECK(catalog->addOrReplace({
        "maps/dedicated-arena", "content-1", scenePath.string(),
        "ArenaAuthority"}));

    uint32_t preparedWorlds = 0;
    uint32_t deactivatedWorlds = 0;
    online::DedicatedSceneHostConfig worldConfig;
    worldConfig.contentResolver = catalog;
    worldConfig.maximumWorlds = 1;
    worldConfig.prepareWorld = [&](const DedicatedAllocation& allocation,
                                   ayt::scene::Scene& scene,
                                   std::string&) {
        ++preparedWorlds;
        return allocation.content.contentSeed == 0xA11A7u &&
               scene.name() == "ArenaAuthority";
    };
    worldConfig.deactivateWorld = [&](const DedicatedAllocation&,
                                      ayt::scene::Scene&) {
        ++deactivatedWorlds;
    };
    auto worlds = std::make_shared<online::DedicatedSceneHost>(
        std::move(worldConfig));
    auto service = std::make_shared<InMemoryOnlineServices>(
        InMemoryOnlineServicesConfig{});

    std::unique_ptr<INetworkSubSystem> server(createNetworkSubSystemForTest());
    std::unique_ptr<INetworkSubSystem> clientA(createNetworkSubSystemForTest());
    std::unique_ptr<INetworkSubSystem> clientB(createNetworkSubSystemForTest());
    CHECK(server && clientA && clientB);
    if (!server || !clientA || !clientB) return;
    CHECK(server->initialize());
    CHECK(clientA->initialize());
    CHECK(clientB->initialize());

    const uint16_t gamePort = reserveUdpPort();
    CHECK(gamePort != 0);
    if (gamePort == 0) return;
    DedicatedServerRuntimeConfig runtimeConfig;
    runtimeConfig.registration = {
        "scene-backed-runtime", "test", "build-1", "127.0.0.1",
        gamePort, 2};
    runtimeConfig.heartbeatIntervalMs = 20;
    runtimeConfig.allocationPollIntervalMs = 5;
    runtimeConfig.retryIntervalMs = 5;
    runtimeConfig.drainTimeoutMs = 1000;
    DedicatedServerRuntime runtime(
        *server, service, worlds, std::move(runtimeConfig));
    CHECK(runtime.start());

    DedicatedAllocationRequest request;
    request.region = "test";
    request.buildId = "build-1";
    request.playerCount = 2;
    request.players = {PeerId{"scene-player-a"},
                       PeerId{"scene-player-b"}};
    request.matchId = 99;
    request.content = {"maps/dedicated-arena", "content-1", 0xA11A7u};
    const auto allocated = service->allocateServer(request);
    CHECK(allocated);
    if (!allocated) return;
    CHECK(pumpUntil(runtime, nullptr, nullptr, [&] {
        return worlds->findScene(allocated.value.allocationId) != nullptr;
    }));
    CHECK_INT_EQ(preparedWorlds, 1);

    NetworkDedicatedSessionConnectorConfig clientAConfig;
    clientAConfig.localPeerId = PeerId{"scene-player-a"};
    clientAConfig.connectTimeoutMs = 2000;
    NetworkDedicatedSessionConnector connectorA(*clientA, clientAConfig);
    NetworkDedicatedSessionConnectorConfig clientBConfig;
    clientBConfig.localPeerId = PeerId{"scene-player-b"};
    clientBConfig.connectTimeoutMs = 2000;
    NetworkDedicatedSessionConnector connectorB(*clientB, clientBConfig);

    DedicatedAllocation allocationA = allocated.value;
    DedicatedAllocation allocationB = allocated.value;
    CHECK(deriveDedicatedAdmissionToken(
        allocated.value.reservationToken, clientAConfig.localPeerId,
        allocationA.reservationToken));
    CHECK(deriveDedicatedAdmissionToken(
        allocated.value.reservationToken, clientBConfig.localPeerId,
        allocationB.reservationToken));
    CHECK(allocationA.reservationToken != allocationB.reservationToken);
    CHECK(connectorA.connect(allocationA));
    CHECK(connectorB.connect(allocationB));
    CHECK(pumpUntil(runtime, &connectorA, &connectorB, [&] {
        return connectorA.getStatus().state ==
                   DedicatedSessionConnectionState::Active &&
               connectorB.getStatus().state ==
                   DedicatedSessionConnectionState::Active &&
               worlds->getStatus().connectedPlayers == 2;
    }));
    CHECK_INT_EQ(runtime.getStatus().connectedPlayers, 2);
    const auto* activeScene = worlds->findScene(
        allocated.value.allocationId);
    CHECK(activeScene != nullptr);
    if (activeScene) CHECK(activeScene->tickCount() > 0);

    connectorA.disconnect();
    CHECK(pumpUntil(runtime, nullptr, &connectorB, [&] {
        return worlds->getStatus().connectedPlayers == 1;
    }));
    CHECK(worlds->findScene(allocated.value.allocationId) != nullptr);

    connectorB.disconnect();
    CHECK(pumpUntil(runtime, nullptr, nullptr, [&] {
        return worlds->findScene(allocated.value.allocationId) == nullptr;
    }));
    CHECK_INT_EQ(deactivatedWorlds, 1);

    runtime.beginDrain();
    CHECK(pumpUntil(runtime, nullptr, nullptr, [&] {
        return runtime.getStatus().state ==
               DedicatedServerRuntimeState::Stopped;
    }));
    clientB->shutdown();
    clientA->shutdown();
    server->shutdown();
    std::filesystem::remove(scenePath);
}

TEST_SUITE_END

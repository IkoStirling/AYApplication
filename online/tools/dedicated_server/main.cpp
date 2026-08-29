// AYApplication_DedicatedServer - scene-backed Headless Dedicated host.

#include <AYOnlineApplication/DedicatedApplication.h>

#include <AYEntity/EntityModule.h>
#include <AYNetwork/NetworkModule.h>
#include <AYNetwork/Session/HttpOnlineServices.h>
#include <AYScene.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace
{

volatile std::sig_atomic_t g_draining = 0;
void onSignal(int) { g_draining = 1; }

bool parsePositive(const char* text, uint16_t& value) {
    if (!text) return false;
    unsigned parsed = 0;
    const std::string input{text};
    const auto result = std::from_chars(
        input.data(), input.data() + input.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
        parsed == 0 || parsed > 65535) return false;
    value = static_cast<uint16_t>(parsed);
    return true;
}

std::string environment(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        std::free(value);
        return {};
    }
    std::string result{value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 10) {
        std::fprintf(stderr,
            "usage: AYApplication_DedicatedServer <backend-address> "
            "<backend-port> <instance> <region> <build> <public-address> "
            "<game-port> <capacity> <content-catalog>\n");
        return 2;
    }
    uint16_t backendPort = 0;
    uint16_t gamePort = 0;
    uint16_t capacity = 0;
    if (!parsePositive(argv[2], backendPort) ||
        !parsePositive(argv[7], gamePort) ||
        !parsePositive(argv[8], capacity)) {
        std::fprintf(stderr, "invalid Dedicated Server port or capacity\n");
        return 2;
    }
    const std::string controlToken = environment("AY_ONLINE_SERVER_TOKEN");
    if (controlToken.size() < 32) {
        std::fprintf(stderr,
            "AY_ONLINE_SERVER_TOKEN must contain the fleet control token\n");
        return 2;
    }

    auto catalog = std::make_shared<ayt::app::online::OnlineContentCatalog>();
    std::string catalogError;
    if (!ayt::app::online::loadOnlineContentCatalog(
            argv[9], *catalog, catalogError)) {
        std::fprintf(stderr, "failed to load content catalog: %s\n",
                     catalogError.c_str());
        return 2;
    }

    size_t maximumWorlds = capacity;
    const std::string maximumWorldsText = environment(
        "AY_DEDICATED_MAX_WORLDS");
    if (!maximumWorldsText.empty()) {
        uint16_t parsed = 0;
        if (!parsePositive(maximumWorldsText.c_str(), parsed)) {
            std::fprintf(stderr, "AY_DEDICATED_MAX_WORLDS is invalid\n");
            return 2;
        }
        maximumWorlds = parsed;
    }

    // Registers component factories needed by .ayscene deserialization without
    // registering Device, Audio, Renderer, or a presentation GameLoop in the
    // headless process. Game executables can replace this entry point and add
    // authority-only systems in DedicatedSceneHostConfig::prepareWorld.
    ayt::entity::registerEntityComponents();

    ayt::app::online::DedicatedSceneHostConfig worldConfig;
    worldConfig.contentResolver = catalog;
    worldConfig.maximumWorlds = maximumWorlds;
    worldConfig.prepareWorld = [](
        const ayt::net::DedicatedAllocation& allocation,
        ayt::scene::Scene& scene, std::string&) {
        std::printf(
            "AY_DEDICATED_WORLD state=loaded allocation=%llu match=%llu "
            "content=%s version=%s seed=%llu scene=%s path=%s\n",
            static_cast<unsigned long long>(allocation.allocationId),
            static_cast<unsigned long long>(allocation.matchId),
            allocation.content.contentId.c_str(),
            allocation.content.contentVersion.c_str(),
            static_cast<unsigned long long>(allocation.content.contentSeed),
            scene.name().c_str(), scene.path().c_str());
        std::fflush(stdout);
        return true;
    };
    worldConfig.deactivateWorld = [](
        const ayt::net::DedicatedAllocation& allocation,
        ayt::scene::Scene&) {
        std::printf("AY_DEDICATED_WORLD state=unloaded allocation=%llu\n",
                    static_cast<unsigned long long>(allocation.allocationId));
        std::fflush(stdout);
    };
    worldConfig.onPlayerConnected = [](
        const ayt::net::DedicatedAllocation& allocation,
        ayt::scene::Scene&, const ayt::net::PeerId& peer,
        ayt::net::NetConnection*) {
        std::printf(
            "AY_DEDICATED_PLAYER state=connected allocation=%llu peer=%s\n",
            static_cast<unsigned long long>(allocation.allocationId),
            peer.value.c_str());
        std::fflush(stdout);
    };
    worldConfig.onPlayerDisconnected = [](
        const ayt::net::DedicatedAllocation& allocation,
        ayt::scene::Scene&, const ayt::net::PeerId& peer) {
        std::printf(
            "AY_DEDICATED_PLAYER state=disconnected allocation=%llu peer=%s\n",
            static_cast<unsigned long long>(allocation.allocationId),
            peer.value.c_str());
        std::fflush(stdout);
    };
    auto worlds = std::make_shared<ayt::app::online::DedicatedSceneHost>(
        std::move(worldConfig));

    ayt::net::HttpOnlineServicesClientConfig backendConfig;
    backendConfig.serverAddress = argv[1];
    backendConfig.serverPort = backendPort;
    backendConfig.useTls = environment("AY_ONLINE_BACKEND_TLS") == "1" ||
        environment("AY_ONLINE_BACKEND_TLS") == "true";
    backendConfig.dedicatedControlToken = controlToken;
    auto backend = std::make_shared<ayt::net::HttpOnlineServices>(
        std::move(backendConfig));

    ayt::net::registerNetworkSubSystem();
    auto* network = ayt::net::findRegisteredNetworkSubSystem();
    if (!network || !network->initialize()) {
        std::fprintf(stderr, "failed to initialize Dedicated network runtime\n");
        return 1;
    }

    ayt::net::DedicatedServerRuntimeConfig runtimeConfig;
    runtimeConfig.registration = {
        argv[3], argv[4], argv[5], argv[6], gamePort, capacity};
    ayt::net::DedicatedServerRuntime runtime(
        *network, backend, worlds, std::move(runtimeConfig));
    if (!runtime.start()) {
        const auto status = runtime.getStatus();
        std::fprintf(stderr, "failed to start Dedicated runtime: %s\n",
                     status.message.c_str());
        network->shutdown();
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::printf(
        "AY_DEDICATED_SERVER state=ready instance=%s region=%s build=%s "
        "address=%s port=%u capacity=%u max_worlds=%zu catalog=%s\n",
        argv[3], argv[4], argv[5], argv[6], gamePort, capacity,
        maximumWorlds, argv[9]);
    std::fflush(stdout);

    auto previous = std::chrono::steady_clock::now();
    bool drainStarted = false;
    int exitCode = 0;
    while (true) {
        const auto current = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(
            current - previous).count();
        previous = current;
        if (g_draining != 0 && !drainStarted) {
            drainStarted = true;
            runtime.beginDrain();
            std::printf("AY_DEDICATED_SERVER state=draining\n");
        }
        runtime.update(delta);
        const auto status = runtime.getStatus();
        if (status.state == ayt::net::DedicatedServerRuntimeState::Stopped) {
            break;
        }
        if (status.state == ayt::net::DedicatedServerRuntimeState::Failed) {
            const auto worldStatus = worlds->getStatus();
            std::fprintf(stderr,
                "AY_DEDICATED_SERVER state=failed error=%u service_error=%u "
                "message=%s world_error=%u world_message=%s\n",
                static_cast<unsigned>(status.error),
                static_cast<unsigned>(status.serviceError),
                status.message.c_str(),
                static_cast<unsigned>(worldStatus.lastError),
                worldStatus.message.c_str());
            exitCode = 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runtime.stop();
    network->shutdown();
    std::printf("AY_DEDICATED_SERVER state=stopped\n");
    return exitCode;
}

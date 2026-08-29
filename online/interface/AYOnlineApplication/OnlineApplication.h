#pragma once
// Optional AYApplication + AYNetwork integration. AYApplication core remains
// usable without linking the network stack.

#include <AYApplication/IEngineHost.h>
#include <AYApplication/RuntimeSceneLoader.h>
#include <AYNetwork/Session/OnlineFlowSubSystem.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::scene { class Scene; }

namespace ayt::app::online
{

inline constexpr const char* kHostServiceOnlineFlow =
    "ayt.net.OnlineFlowCoordinator";
inline constexpr const char* kHostServiceOnlineApplication =
    "ayt.app.online.OnlineApplication";

struct OnlineContentResolveResult {
    std::string scenePath;
    std::string sceneName;
    std::string message;

    bool isValid() const { return !scenePath.empty(); }
};

class IOnlineContentResolver {
public:
    virtual ~IOnlineContentResolver() = default;
    virtual OnlineContentResolveResult resolve(
        const ::ayt::net::OnlineContentDescriptor& content) const = 0;
};

struct OnlineContentMapping {
    std::string contentId;
    std::string contentVersion;
    std::string scenePath;
    std::string sceneName;

    bool isValid() const;
};

// Small exact-version catalog suitable for the first integration. Projects
// can replace it with an asset manifest/downloader by implementing the same
// resolver interface.
class OnlineContentCatalog final : public IOnlineContentResolver {
public:
    bool addOrReplace(OnlineContentMapping mapping);
    OnlineContentResolveResult resolve(
        const ::ayt::net::OnlineContentDescriptor& content) const override;

private:
    std::vector<OnlineContentMapping> _mappings;
};

struct OnlineSceneBridgeConfig {
    std::shared_ptr<IOnlineContentResolver> contentResolver;

    // Trusted local target used after leave/sign-out/failure. Backend payloads
    // never supply this filesystem path.
    std::string mainMenuScenePath;
    std::string mainMenuSceneName = "MainMenu";

    // Runs on the staged Scene before activation. Use it to apply contentSeed,
    // register mode-specific systems, or validate required assets.
    std::function<bool(const ::ayt::net::OnlineContentDescriptor&,
                       ::ayt::scene::Scene&,
                       std::string&)> prepareSessionScene;

    // Scene-owned replication/input registrations must not run while the
    // transport is reconnecting or while the session is being torn down.
    // suspend/deactivate are idempotently invoked at most once per lifecycle
    // edge. deactivate runs while the old Scene is still alive.
    std::function<void(::ayt::scene::Scene&)> suspendSessionScene;
    std::function<bool(::ayt::scene::Scene&,
                       uint64_t recoveryGeneration,
                       uint32_t sessionEpoch,
                       std::string&)> resumeSessionScene;
    std::function<void(::ayt::scene::Scene&)> deactivateSessionScene;

    bool isValid() const;
};

struct OnlineSceneBridgeStatus {
    bool ready = false;
    bool loadPending = false;
    bool sessionSceneActive = false;
    bool sessionSceneSuspended = false;
    bool sessionSceneDeactivated = false;
    bool mainMenuRecoveryRequired = false;
    uint64_t sceneRequestId = 0;
    uint64_t flowGeneration = 0;
    uint64_t recoveryGeneration = 0;
    uint64_t activeSessionId = 0;
    uint32_t activeSessionEpoch = 0;
    ::ayt::net::OnlineContentDescriptor activeContent;
    std::string activeScenePath;
    std::string lastError;
};

class OnlineSceneBridge {
public:
    OnlineSceneBridge(::ayt::net::OnlineFlowCoordinator& flow,
                      ::ayt::app::IRuntimeSceneLoader& loader,
                      OnlineSceneBridgeConfig config,
                      ::ayt::event::EventBus& eventBus);
    ~OnlineSceneBridge();

    OnlineSceneBridge(const OnlineSceneBridge&) = delete;
    OnlineSceneBridge& operator=(const OnlineSceneBridge&) = delete;

    bool initialize();
    void update();
    bool retryMainMenuScene();
    void shutdown();
    OnlineSceneBridgeStatus getStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

struct OnlineApplicationConfig {
    ::ayt::net::OnlineSubSystemConfig online;
    ::ayt::net::OnlineFlowConfig flow;
    OnlineSceneBridgeConfig scenes;

    bool isValid() const;
};

class IOnlineApplicationSubSystem : public ::ayt::game::ISubSystem {
public:
    ~IOnlineApplicationSubSystem() override = default;
    virtual bool isReady() const = 0;
    virtual ::ayt::net::OnlineFlowCoordinator* flow() = 0;
    virtual const ::ayt::net::OnlineFlowCoordinator* flow() const = 0;
    virtual OnlineSceneBridgeStatus getBridgeStatus() const = 0;
    virtual bool retryMainMenuScene() = 0;
};

// Register from IApplication::onInit(), before GameLoop startup. The normal
// client module assembly registers RuntimeSceneLoader afterwards; lifecycle
// dependencies ensure initialization still occurs in the correct order.
bool registerOnlineApplication(
    ::ayt::app::IEngineHost& host,
    OnlineApplicationConfig config,
    ::ayt::net::OnlineSubSystemDependencies dependencies = {});
bool registerOnlineApplication(
    OnlineApplicationConfig config,
    ::ayt::net::OnlineSubSystemDependencies dependencies = {});

IOnlineApplicationSubSystem* findRegisteredOnlineApplicationSubSystem();

} // namespace ayt::app::online

# Online application and runtime scene integration

`AYOnlineApplication` is an optional bridge target. It connects AYNetwork's
backend-aware online flow to AYApplication's frame-boundary scene loader while
keeping `AYApplication` itself independent of GameNetworkingSockets and online
backend libraries.

Game targets that opt in link `AYOnlineApplication`; targets that only link
`AYApplication` retain the original dependency footprint.

## Runtime boundaries

- `Online`, `OnlineFlow`, and `OnlineApplication` run in `Ingress`.
- `RuntimeSceneLoader` commits a successfully staged Play Scene in `Egress`.
- Failed parsing or activation leaves the previous current Scene intact.
- `OnlineFlowCoordinator::completeLoading()` is called only after the scene is
  current and its activation preparation has succeeded.
- Leaving, signing out, or a terminal online failure loads the trusted local
  main-menu path. A backend response can never provide a filesystem path.
- A failed main-menu restore is not retried every frame. Diagnostics expose
  `mainMenuRecoveryRequired`; UI may call `retryMainMenuScene()` after repairing
  local content or storage.

## Session-scene recovery lifecycle

Projects bind scene-owned replication, input, and gameplay services through
three optional `OnlineSceneBridgeConfig` callbacks:

- `suspendSessionScene` stops scene-owned work when the transport begins a
  reconnect/migration or session teardown;
- `resumeSessionScene` rebuilds those bindings after transport recovery and
  receives the monotonic recovery generation plus the current authority epoch;
- `deactivateSessionScene` releases all remaining scene pointers before the
  loader replaces or destroys the old Scene.

The bridge ignores stale queued status events. A changed authority epoch is
treated as a recovery edge even if the transport reports `InSession` without
an observable intermediate state. If resume fails, if the recovered transport
belongs to another session, or if another system replaces the active Scene,
the flow reports `WorldFailed`, tears down the network session, and restores
the trusted local MainMenu. AYNetwork itself remains responsible for resetting
replication authority state; the application callbacks only rebuild
scene-owned registrations.

## Content identity

Lobby and matchmaking contracts carry `OnlineContentDescriptor`:

- `contentId`: project-defined logical asset identity, for example
  `maps/arena`;
- `contentVersion`: exact installed-content/build version;
- `contentSeed`: deterministic session input applied before activation.

`OnlineContentCatalog` is the first local resolver. It matches ID and version
exactly and returns a local `.ayscene` path. Larger projects can implement
`IOnlineContentResolver` with an asset manifest, entitlement check, patcher, or
streaming installer without changing the network protocol.

## Registration

Register the optional assembly from `IApplication::onInit()`. The normal client
module registration happens later; GameLoop lifecycle dependencies still
initialize `RuntimeSceneLoader` before `OnlineApplication`.

```cpp
#include <AYOnlineApplication/OnlineApplication.h>

void GameApplication::onInit()
{
    using namespace ayt::app::online;

    auto catalog = std::make_shared<OnlineContentCatalog>();
    catalog->addOrReplace({
        "maps/arena", "content-1",
        "Content/Scenes/Arena.ayscene", "Arena"});

    OnlineApplicationConfig config;
    config.online.localPeerId = ayt::net::PeerId("local-player-id");
    config.online.backend.serverAddress = "42.240.149.148";
    config.online.backend.serverPort = 28081;
    config.scenes.contentResolver = catalog;
    config.scenes.mainMenuScenePath =
        "Content/Scenes/MainMenu.ayscene";
    config.scenes.prepareSessionScene =
        [](const ayt::net::OnlineContentDescriptor& content,
           ayt::scene::Scene& scene,
           std::string& error) {
            // Apply content.contentSeed and project-specific world services.
            (void)content;
            (void)scene;
            (void)error;
            return true;
        };
    config.scenes.suspendSessionScene =
        [](ayt::scene::Scene& scene) {
            // Pause scene-owned network input and replication bindings.
            (void)scene;
        };
    config.scenes.resumeSessionScene =
        [](ayt::scene::Scene& scene, uint64_t recoveryGeneration,
           uint32_t authorityEpoch, std::string& error) {
            // Rebind scene-owned consumers to the recovered session.
            (void)scene;
            (void)recoveryGeneration;
            (void)authorityEpoch;
            (void)error;
            return true;
        };
    config.scenes.deactivateSessionScene =
        [](ayt::scene::Scene& scene) {
            // Drop all registrations that retain Scene/World addresses.
            (void)scene;
        };

    if (!registerOnlineApplication(engineHost(), std::move(config))) {
        throw std::runtime_error("online application registration failed");
    }
}
```

After GameLoop initialization, UI/game code can retrieve the application-facing
flow without owning subsystem lifetimes:

```cpp
auto* flow = engineHost().service<ayt::net::OnlineFlowCoordinator>(
    ayt::app::online::kHostServiceOnlineFlow);
flow->signIn(playerAccessToken, p2pAdmissionToken);
```

The player access token and P2P admission token stay in pull/config APIs; no
scene or online-flow EventBus payload contains either secret.

## Runnable vertical slice

`OnlineVerticalSliceTest` is the executable first-integration reference.  It
uses the real in-memory Lobby/session services, `IOnlineSubSystem`,
`OnlineFlowCoordinator`, `OnlineSceneBridge`, `RuntimeSceneLoader`, and the
AYReflect replication wire path in one deterministic process.  The scenario
is:

1. initialize a trusted local MainMenu scene;
2. sign in, create a two-player Lobby, and admit a remote member;
3. launch the Lobby with an exact content ID/version/seed assignment;
4. resolve and atomically activate the Arena scene;
5. spawn and update a reflected network ghost from authority to client;
6. leave the session and restore MainMenu.

The companion bridge cases reject an uninstalled content version, preserve
MainMenu when the `.ayscene` file cannot be loaded, and cancel the pending
scene request when the online loading deadline expires.  The loopback
replication sink is a test seam only; packaged games keep the same
`ReplicationManager` calls and let the selected AYNetwork transport carry the
sealed frames.

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

## Scene-backed Dedicated authority

`DedicatedSceneHost` implements AYNetwork's `IDedicatedWorldHost` at the
application layer. Each backend allocation resolves its logical content
identity through the same trusted catalog, loads an independent Play `Scene`
and `World`, ticks it from `DedicatedServerRuntime`, and releases it only after
the last admitted player leaves or the allocation is fenced by the backend.
Remote assignments never carry a filesystem path.

Projects register authority-only gameplay systems and apply `contentSeed` in
`DedicatedSceneHostConfig::prepareWorld`. Player connect/disconnect callbacks
are the binding points for scene-owned replication and gameplay state. The
library default for `maximumWorlds` is one because older systems that call the
process-wide `World::instance()` are not multi-world safe. The generic server
raises it to the registered player capacity so the backend can place several
small matches in one process; set `AY_DEDICATED_MAX_WORLDS=1` for legacy game
systems, or choose another explicit per-process world limit.

The reusable code is split into `AYOnlineContent` (trusted content mapping),
`AYOnlineApplication` (client scene bridge), and `AYDedicatedApplication`
(headless authority bridge). The Dedicated process does not register client
presentation subsystems. `AYScene` now links `AYEntityCore`; the Dedicated
target adds only `AYEntityNetworkIntegration`, so Renderer, Animation, Audio,
Device, Physics and Script libraries are absent unless the product explicitly
selects them.

`AYApplication_DedicatedServer` is the deployable generic entry point. Its
catalog is a trusted local tab-separated file:

```text
# content-id<TAB>version<TAB>scene-path<TAB>scene-name
maps/arena	content-1	Content/Scenes/Arena.ayscene	ArenaAuthority
```

Relative scene paths resolve against the catalog directory. Start it with the
same fleet token used by the SessionServer Dedicated routes:

```powershell
$env:AY_ONLINE_SERVER_TOKEN = "<fleet-token>"
$env:AY_ONLINE_BACKEND_TLS = "true"
.\AYApplication_DedicatedServer.exe `
  api.example.com 443 ds-sg-01 asia build-42 `
  203.0.113.20 7777 16 .\content-catalog.tsv
```

The generic executable registers component factories and loads/ticks real
scenes. A game-specific executable should reuse `DedicatedSceneHost` and add
its script, physics, replication, and authority systems in `prepareWorld`.

## Registration

Add the optional stack through `GameDesc::configureModules`. The callback runs
after the default Client graph has contributed `RuntimeSceneLoader`, but before
module dependency resolution and type registration. Ordinary clients therefore
do not acquire AYNetwork or online backend startup work.

```cpp
#include <AYApplication.h>
#include <AYOnlineApplication/OnlineApplicationRuntimeModule.h>

ayt::app::GameDesc makeGameDesc()
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

    ayt::app::GameDesc desc;
    desc.name = "OnlineGame";
    desc.enablePresentation = true;
    desc.configureModules = [config = std::move(config)](
        ayt::app::EngineModuleRuntime& runtime) {
        return configureOnlineApplicationModules(runtime, config);
    };
    return desc;
}
```

`configureOnlineApplicationModules()` adds or adopts the stable chain
`AYNetwork.Runtime -> AYNetwork.Online -> AYNetwork.OnlineFlow ->
AYOnlineApplication.Runtime`. `registerOnlineApplication()` remains available
for legacy direct-GameLoop composition and standalone demos.

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

`DedicatedOnlineVerticalSliceTest` is the server-side companion: it loads a
real Arena Scene, starts the real GNS Dedicated listener, derives distinct
per-player admission credentials, connects two clients, verifies that the
world survives the first departure, and releases it after the final client
leaves before draining the server.

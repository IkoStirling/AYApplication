#pragma once
// AYApplication/RegisterDefaultModules.h — default SubSystem assembly tables
//
// Client / Server module graphs live here. Editor graph composition lives in
// AYEditor (configureDefaultEditorModules).
// See docs/engine-host.md for the assembly tables.

#include <AYApplication/RuntimeSceneLoader.h>
#include <AYModule/ModuleTypes.h>

#ifndef AY_APPLICATION_HAS_AUDIO
#define AY_APPLICATION_HAS_AUDIO 0
#endif
#ifndef AY_APPLICATION_HAS_DEVICE
#define AY_APPLICATION_HAS_DEVICE 0
#endif
#ifndef AY_APPLICATION_HAS_PRESENTATION
#define AY_APPLICATION_HAS_PRESENTATION 0
#endif
#ifndef AY_APPLICATION_HAS_2D
#define AY_APPLICATION_HAS_2D 0
#endif
#ifndef AY_APPLICATION_HAS_PHYSICS
#define AY_APPLICATION_HAS_PHYSICS 0
#endif
#ifndef AY_APPLICATION_HAS_SCRIPT
#define AY_APPLICATION_HAS_SCRIPT 0
#endif
#ifndef AY_APPLICATION_HAS_NETWORK
#define AY_APPLICATION_HAS_NETWORK 0
#endif

namespace ayt::physics
{
struct PhysicsBackendDescriptor;
}

namespace ayt::app
{

class EngineModuleRuntime;

inline constexpr bool kApplicationHasAudio = AY_APPLICATION_HAS_AUDIO != 0;
inline constexpr bool kApplicationHasDevice = AY_APPLICATION_HAS_DEVICE != 0;
inline constexpr bool kApplicationHasPresentation =
    AY_APPLICATION_HAS_PRESENTATION != 0;
inline constexpr bool kApplicationHas2D = AY_APPLICATION_HAS_2D != 0;
inline constexpr bool kApplicationHasPhysics = AY_APPLICATION_HAS_PHYSICS != 0;
inline constexpr bool kApplicationHasScript = AY_APPLICATION_HAS_SCRIPT != 0;
inline constexpr bool kApplicationHasNetwork = AY_APPLICATION_HAS_NETWORK != 0;

struct ClientModuleOptions {
    const char* windowTitle = "Untitled";
    int windowWidth = 1280;
    int windowHeight = 720;
    /// When false, skip AudioSubSystem (CLI `-no-audio`).
    bool enableAudio = kApplicationHasAudio;
    /// When true, register full ECS systems (animation/render/2D) + RendererSubSystem.
    /// Default false keeps headless-safe Client (Entity core only).
    bool enablePresentation = false;
    /// Script is a build-time capability and a runtime assembly choice.
    bool enableScript = kApplicationHasScript;
    /// When true, register PhysicsSubSystem (fixedUpdate step).
    bool enablePhysics = kApplicationHasPhysics;
    /// Client-only frame-boundary scene loader. It is a module node but still
    /// uses the Host-owned SceneManager and EventBus services.
    bool enableRuntimeSceneLoader = true;
    RuntimeSceneLoaderConfig runtimeSceneLoaderConfig{};
};

struct ServerModuleOptions {
    /// When true, register ScriptSubSystem (Logia). Server can run headless without it.
    bool enableScript = kApplicationHasScript;
    /// When true, register PhysicsSubSystem (fixedUpdate step).
    bool enablePhysics = kApplicationHasPhysics;
};

/// Compatibility helper: `bootstrapModule()` + `RendererSubSystem`.
/// New Host composition should use configureDefault*Modules().
void registerEntityPresentationStack();

/// Compatibility helper for direct GameLoop registration.
void registerPhysicsModule();
void registerPhysicsModule(const ayt::physics::PhysicsBackendDescriptor& desc);

/// Compatibility wrapper for direct GameLoop registration. Production
/// ApplicationImpl uses configureDefaultClientModules().
void registerDefaultClientModules(const ClientModuleOptions& options);

/// Compatibility wrapper for direct headless registration. Production
/// ApplicationImpl uses configureDefaultServerModules().
void registerDefaultServerModules(const ServerModuleOptions& options);

/// Add the Client startup graph to `runtime` without executing it. The Host
/// then calls prepare(), seals registries, and calls install().
[[nodiscard]] ayt::module::ModuleResult configureDefaultClientModules(
    EngineModuleRuntime& runtime,
    const ClientModuleOptions& options);

/// Add the headless Server startup graph to `runtime` without executing it.
[[nodiscard]] ayt::module::ModuleResult configureDefaultServerModules(
    EngineModuleRuntime& runtime,
    const ServerModuleOptions& options);

} // namespace ayt::app

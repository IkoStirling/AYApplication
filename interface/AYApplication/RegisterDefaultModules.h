#pragma once
// AYApplication/RegisterDefaultModules.h — default SubSystem assembly tables
//
// Client / Server live here. Editor presentation modules live in AYEditor
// (registerDefaultEditorModules), which reuses registerEntityPresentationStack
// and registerPhysicsModule from this header.
// See docs/engine-host.md for the assembly tables.

namespace ayt::physics
{
struct PhysicsBackendDescriptor;
}

namespace ayt::app
{

struct ClientModuleOptions {
    const char* windowTitle = "Untitled";
    int windowWidth = 1280;
    int windowHeight = 720;
    /// When false, skip AudioSubSystem (CLI `-no-audio`).
    bool enableAudio = true;
    /// When true, register full ECS systems (animation/render/2D) + RendererSubSystem.
    /// Default false keeps headless-safe Client (Entity core only).
    bool enablePresentation = false;
    /// When true, register PhysicsSubSystem (fixedUpdate step).
    bool enablePhysics = true;
};

struct ServerModuleOptions {
    /// When true, register ScriptSubSystem (Logia). Server can run headless without it.
    bool enableScript = true;
    /// When true, register PhysicsSubSystem (fixedUpdate step).
    bool enablePhysics = true;
};

/// Shared Game/Editor recipe: `bootstrapModule()` + `RendererSubSystem`.
/// Does not register Device / Script / Audio / Network / Physics (caller owns those).
void registerEntityPresentationStack();

/// Register PhysicsSubSystem into GameLoop (idempotent if already registered).
void registerPhysicsModule();
void registerPhysicsModule(const ayt::physics::PhysicsBackendDescriptor& desc);

/// Device + EntityCore (+ optional presentation) + Script + optional Audio + optional Physics.
/// Does not wire Script←DeviceInputProvider (caller owns that lifetime).
void registerDefaultClientModules(const ClientModuleOptions& options);

/// Headless Server: EntityCore + optional Script + optional Physics.
/// Skips Device / Audio / Renderer.
void registerDefaultServerModules(const ServerModuleOptions& options);

} // namespace ayt::app

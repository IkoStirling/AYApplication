#pragma once
// AYRegisterDefaultModules.h — Client default SubSystem assembly (Step 1)
//
// Editor presentation modules live in AYEditor (registerDefaultEditorModules),
// which reuses registerEntityPresentationStack() from this header (P1 shared boot).
// See docs/engine-host.md for the assembly tables.

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
};

/// Shared Game/Editor recipe: `bootstrapModule()` + `RendererSubSystem`.
/// Does not register Device / Script / Audio / Network (caller owns those).
void registerEntityPresentationStack();

/// Device + EntityCore (+ optional presentation) + Script + optional Audio.
/// Does not wire Script←DeviceInputProvider (caller owns that lifetime).
void registerDefaultClientModules(const ClientModuleOptions& options);

} // namespace ayt::app

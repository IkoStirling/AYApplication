#pragma once
// AYRegisterDefaultModules.h — Client default SubSystem assembly (Step 1)
//
// Editor presentation modules live in AYEditor (registerDefaultEditorModules).
// See docs/engine-host.md for the assembly tables.

namespace ayt::app
{

struct ClientModuleOptions {
    const char* windowTitle = "Untitled";
    int windowWidth = 1280;
    int windowHeight = 720;
    /// When false, skip AudioSubSystem (CLI `-no-audio`).
    bool enableAudio = true;
};

/// Device + EntityCore + Script + optional Audio.
/// Does not wire Script←DeviceInputProvider (caller owns that lifetime).
void registerDefaultClientModules(const ClientModuleOptions& options);

} // namespace ayt::app

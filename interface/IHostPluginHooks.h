#pragma once
// IHostPluginHooks.h — lightweight Host extension points (engine-host.md §6)
//
// Plugins / game modules register here to attach services and subscribe to
// SimTickEvent / Scene* events via host.eventBus(). This is the Host-side
// hook surface; dynamic DLL loading remains AYPlugin's job.

namespace ayt::app
{

class IEngineHost;

class IHostPluginHooks {
public:
    virtual ~IHostPluginHooks() = default;

    virtual const char* name() const = 0;

    /// Called when registered on a Host. Subscribe to events / provide services.
    virtual void onAttach(IEngineHost& host) = 0;

    /// Called when unregistered or Host tears down plugin table.
    virtual void onDetach(IEngineHost& host) = 0;
};

} // namespace ayt::app

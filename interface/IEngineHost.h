#pragma once
// IEngineHost.h — thin engine-shell façade (Step 1)
//
// Wraps process singletons already used by Application/Editor.
// Service APIs (resources/physics/audio) come in Host v2 — see docs/engine-host.md.

#include <IAYGameLoop.h>

namespace ayt::event
{
class EventBus;
}

namespace ayt::app
{

/// Process-scoped engine host: assembly + lookup entry (not a gameplay module).
class IEngineHost {
public:
    virtual ~IEngineHost() = default;

    virtual ayt::game::IGameLoop& gameLoop() = 0;
    virtual ayt::event::EventBus& eventBus() = 0;

    /// Named GameLoop subsystem, or nullptr if not registered.
    virtual ayt::game::ISubSystem* findSubSystem(const char* name) = 0;
};

/// Default process-wide host (adapters over GameLoop / EventBus / SubSystemRegistry).
IEngineHost& defaultEngineHost();

/// Host installed for the active Application::run (nullptr outside scope).
IEngineHost* currentEngineHost();

/// Prefer EngineHostScope; raw set is for tests.
void setCurrentEngineHost(IEngineHost* host);

/// RAII: sets currentEngineHost for the duration of Application::run.
class EngineHostScope {
public:
    explicit EngineHostScope(IEngineHost& host);
    ~EngineHostScope();

    EngineHostScope(const EngineHostScope&) = delete;
    EngineHostScope& operator=(const EngineHostScope&) = delete;

private:
    IEngineHost* _previous = nullptr;
};

} // namespace ayt::app

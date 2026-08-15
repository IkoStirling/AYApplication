#pragma once
// AYApplication/AppEventHost.h - Host-side EventBus lifecycle scope (Phase 4 lesson encoded)
//
// Why this exists
// ---------------
// During AYEventSystem Phase 4 (commit a8c8be9 in AYGameLoop) we removed
//   EventBus::instance().unsubscribeAll()
// from GameLoop::shutdown() because it caused a shutdown-time SIGSEGV
// (the EventBus process-singleton's static-deinit was racing the cleanup).
// The lesson, recorded in [[ay-event-system]] memory:
//
//   "A borrower of a process-wide singleton should NOT clean up that
//    singleton's contents during its own destructor. EventBus is shared
//    across modules; per-host-app cleanup belongs to the host."
//
// This header implements that host-side cleanup. EventBusHostScope owns a
// list of bus-issued ConnectionIds (raw integers, NOT ScopedConnection
// objects) plus a reference to the bus. Cleanup is explicit — call
// disconnect() during shutdown to release each id. The destructor is a
// genuine no-op: it drops the in-process vector (which only owns ints and a
// bus pointer) WITHOUT touching the bus's listener tables.
//
// Why not `std::vector<ScopedConnection>` ?
// ----------------------------------------
// ScopedConnection's destructor calls EventBus::unsubscribe. A vector of
// them, on its own destructor, would do exactly what Phase 4 forbids:
// reach into the bus during the host's destruction. We deliberately hold
// the raw ids instead, and route unsubscribe through an explicit
// disconnect() call. This is the same shape the AYGameLoop cleanup hook
// uses (see AYGameLoop/GameLoopImpl.h onGameEvent) — only RAII wrapper removed.
//
// Forgotten disconnect() now has a SAFE failure mode: listeners stay
// registered past host shutdown. The process-singleton EventBus cleans
// them up when it is itself torn down at static-deinit; the order of
// destruction relative to a forgotten host scope doesn't matter because
// neither side touches the other.

#include <AYEventSystem/Connection.h>
#include <cstddef>
#include <functional>
#include <vector>

namespace ayt::app
{

// =============================================================================
// EventBusHostScope
//
// Owns a list of bus-issued ConnectionIds for the host application.
// `subscribe<T>(...)` registers a typed handler on EventBus::instance()
// and stores the returned id. `disconnect()` releases all stored ids in
// LIFO order. The destructor is a deliberate no-op.
//
// Threading: host-main-thread only. Subscribes / disconnects must happen
// on the same thread that is allowed to call EventBus::emit (the main
// thread per AYEventSystem §3.5). Handlers may post to any thread inside
// their own body — that's the host's design responsibility.
//
// Anti-goals respected:
//   - No std::function::target comparison.
//   - No static listener table — each instance owns its own id vector.
//   - No string-keyed core dispatch — only typed `subscribe<T>`.
//   - No factory registry pattern.
//   - Destructor never dereferences the bus.
// =============================================================================
class EventBusHostScope {
public:
    EventBusHostScope() = default;

    // Move-only: ownership of the id list transfers. The moved-from scope
    // becomes empty (id vector is moved; the bus pointer is copied but
    // unused post-move since moved-from has no ids to disconnect).
    EventBusHostScope(EventBusHostScope&& other) noexcept;
    EventBusHostScope& operator=(EventBusHostScope&& other) noexcept;
    EventBusHostScope(const EventBusHostScope&) = delete;
    EventBusHostScope& operator=(const EventBusHostScope&) = delete;

    // Genuine no-op: the id vector's destructor runs (drops ints) but
    // never touches the bus. If a host forgets disconnect(), its listeners
    // stay registered past shutdown — that's the safe failure mode.
    ~EventBusHostScope() = default;

    // Subscribe `listener` to EventBus::instance().subscribe<EventType>()
    // and record the returned id. The listener stays registered until
    // disconnect() (explicit host cleanup) or the EventBus is destroyed.
    //
    // Returns the underlying ConnectionId so callers can correlate with
    // diagnostics. They cannot unregister by id from the scope's vector
    // directly; use disconnect() (all) or move-construct a child scope.
    template <typename EventType>
    ayt::event::ConnectionId subscribe(std::function<void(const EventType&)> listener);

    // Explicit cleanup. Walks the id vector LIFO and calls
    // EventBus::unsubscribe(id) for each stored id. Idempotent.
    void disconnect();

    std::size_t size()  const noexcept { return _ids.size(); }
    bool        empty() const noexcept { return _ids.empty(); }

    // Move all currently stored ids out into a fresh child scope. The
    // child owns them; the original becomes empty.
    EventBusHostScope detach();

private:
    // Raw ids only — no ScopedConnection. See header note above.
    std::vector<ayt::event::ConnectionId> _ids;
};

// =============================================================================
// Template implementation lives inline (host-main-thread only).
// =============================================================================
template <typename EventType>
ayt::event::ConnectionId EventBusHostScope::subscribe(
    std::function<void(const EventType&)> listener)
{
    auto& bus = ayt::event::EventBus::instance();
    auto id  = bus.subscribe<EventType>(std::move(listener));
    _ids.push_back(id);
    return id;
}

} // namespace ayt::app
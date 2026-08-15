// AYAppEventHost.cpp - EventBusHostScope non-template member bodies.
//
// The template body for `subscribe<T>()` lives inline in the header (host-
// main-thread only). This file implements the non-template plumbing:
//   - move ctor / move assign
//   - disconnect() — LIFO unsubscribe, idempotent
//   - detach()     — move ids out into a fresh child scope

#include <AYApplication/AppEventHost.h>
#include <AYEventSystem/EventBus.h>

namespace ayt::app
{

EventBusHostScope::EventBusHostScope(EventBusHostScope&& other) noexcept
    : _ids(std::move(other._ids))
{
    // other._ids is left empty (valid state) by std::vector::move.
}

EventBusHostScope& EventBusHostScope::operator=(EventBusHostScope&& other) noexcept
{
    if (this != &other) {
        // Don't call disconnect() here — we have no guarantees about which
        // thread we're on during an arbitrary move-assignment (which may
        // happen at any point in the host's lifetime). The moved-from
        // scope's ids simply transfer; if the target already had ids,
        // they LEAK — and that's OK, because disconnect() is an explicit
        // host-owned action, not something a moved-over scope should
        // silently trigger. Host code that move-assigns is expected to
        // disconnect() the source first if it cares about cleanup.
        _ids = std::move(other._ids);
    }
    return *this;
}

void EventBusHostScope::disconnect()
{
    // LIFO: pop_back each id and unsubscribe. We look up the bus here
    // (not in the header) so the destructor never has to dereference it.
    // Idempotent: re-pop on an empty vector is a no-op.
    auto& bus = ayt::event::EventBus::instance();
    while (!_ids.empty()) {
        bus.unsubscribe(_ids.back());
        _ids.pop_back();
    }
}

EventBusHostScope EventBusHostScope::detach()
{
    EventBusHostScope child;
    child._ids = std::move(_ids);
    return child;
}

} // namespace ayt::app
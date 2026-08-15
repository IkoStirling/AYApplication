// AYApplication/unittest/Test_EventBusHostScope.cpp
//
// Tests for the host-side EventBus lifecycle scope (Phase 4 §a8c8be9 lesson
// encoded in code). Tests share the process-singleton EventBus — each test
// uses a hand-picked event type so listeners cannot collide.
//
// Phase 4 regression: Dtor_DoesNotTouchBus pins the rule that the
// EventBusHostScope destructor is a deliberate no-op. A SIGSEGV here means
// the destructor is dereferencing the bus after static-deinit — exactly the
// failure mode the memory lesson warns about.

#include <AYApplication/AppEventHost.h>
#include <AYEventSystem/EventBus.h>
#include <AYTest.h>

#include <atomic>
#include <utility>

namespace ayt::app::test
{

// Hand-picked event type per test so other suites (AudioSubsystem, etc.)
// never overwrite our subscriptions.
struct HostScopeTestEvent {
    int payload = 0;
    static constexpr ayt::event::EventTypeId   kTypeId   = 0x0A00'9001;
    static constexpr ayt::event::EventPriority kPriority = ayt::event::EventPriority::Normal;
};

TEST_SUITE(EventBusHostScope)

TEST_CASE(Subscribe_RoutesToHandler) {
    auto& bus = ayt::event::EventBus::instance();
    ayt::app::EventBusHostScope scope;

    int got = 0;
    auto id = scope.subscribe<HostScopeTestEvent>(
        [&got](const HostScopeTestEvent& e) { got = e.payload; });

    bus.emit(HostScopeTestEvent{42});
    CHECK_INT_EQ(got, 42);
    CHECK_FALSE(scope.empty());
    CHECK_INT_EQ(static_cast<int>(scope.size()), 1);
    (void)id;

    scope.disconnect();
}

TEST_CASE(Disconnect_RemovesAllListeners) {
    auto& bus = ayt::event::EventBus::instance();
    ayt::app::EventBusHostScope scope;

    const auto baseline = bus.listenerCount(HostScopeTestEvent::kTypeId);

    scope.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});
    scope.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});
    scope.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});
    CHECK_INT_EQ(static_cast<int>(scope.size()), 3);
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(HostScopeTestEvent::kTypeId)),
                 static_cast<int>(baseline) + 3);

    scope.disconnect();

    CHECK_TRUE(scope.empty());
    CHECK_INT_EQ(static_cast<int>(scope.size()), 0);
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(HostScopeTestEvent::kTypeId)),
                 static_cast<int>(baseline));

    // Idempotent: calling disconnect on an empty scope is a no-op.
    scope.disconnect();
    CHECK_TRUE(scope.empty());
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(HostScopeTestEvent::kTypeId)),
                 static_cast<int>(baseline));
}

TEST_CASE(Disconnect_StopsDelivery) {
    auto& bus = ayt::event::EventBus::instance();
    ayt::app::EventBusHostScope scope;

    std::atomic<int> count{0};
    scope.subscribe<HostScopeTestEvent>(
        [&count](const HostScopeTestEvent&) { count.fetch_add(1); });

    bus.emit(HostScopeTestEvent{1});
    CHECK_INT_EQ(count.load(), 1);

    scope.disconnect();

    bus.emit(HostScopeTestEvent{2});
    // After disconnect(), no further deliveries should arrive.
    CHECK_INT_EQ(count.load(), 1);
}

TEST_CASE(Dtor_DoesNotTouchBus) {
    // Phase 4 regression: the EventBusHostScope destructor is a deliberate
    // no-op. If a host forgets to call disconnect() and the scope is then
    // dropped, listeners MUST survive (instead of crashing via bus
    // static-deinit race). This pins the lesson in code.
    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(HostScopeTestEvent::kTypeId);

    ayt::event::ConnectionId leaked1 = ayt::event::kInvalidConnectionId;
    ayt::event::ConnectionId leaked2 = ayt::event::kInvalidConnectionId;
    {
        ayt::app::EventBusHostScope inner;
        leaked1 = inner.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});
        leaked2 = inner.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});
        // Intentionally NOT calling inner.disconnect().
    } // ~inner runs here; must NOT touch the bus.

    // Phase 4 invariant: listeners survive the scope's dtor because the
    // dtor is a deliberate no-op (no ScopedConnection vector runs, only
    // a raw id vector of ints).
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(HostScopeTestEvent::kTypeId)),
                 static_cast<int>(baseline) + 2);

    // Manual cleanup of the leaked ids so the rest of the suite sees a
    // clean state. We use the raw bus API here because the scope that
    // owned these ids is gone — this models what happens in production
    // when a host forgets disconnect(): the listeners stay registered
    // until either the bus shuts down or someone explicitly releases
    // them by id.
    bus.unsubscribe(leaked1);
    bus.unsubscribe(leaked2);
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(HostScopeTestEvent::kTypeId)),
                 static_cast<int>(baseline));
}

TEST_CASE(MoveTransfer) {
    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(HostScopeTestEvent::kTypeId);

    ayt::app::EventBusHostScope a;
    int calls = 0;
    a.subscribe<HostScopeTestEvent>(
        [&calls](const HostScopeTestEvent&) { ++calls; });
    a.subscribe<HostScopeTestEvent>(
        [&calls](const HostScopeTestEvent&) { ++calls; });

    bus.emit(HostScopeTestEvent{1});
    CHECK_INT_EQ(calls, 2);

    // Move-construct: a becomes empty; b owns the connections.
    ayt::app::EventBusHostScope b = std::move(a);
    CHECK_TRUE(a.empty());
    CHECK_INT_EQ(static_cast<int>(b.size()), 2);

    bus.emit(HostScopeTestEvent{2});
    CHECK_INT_EQ(calls, 4); // both listeners still fire (now owned by b)

    b.disconnect();
    CHECK_TRUE(b.empty());
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(HostScopeTestEvent::kTypeId)),
                 static_cast<int>(baseline));

    bus.emit(HostScopeTestEvent{3});
    CHECK_INT_EQ(calls, 4); // no further deliveries
}

TEST_CASE(MoveAssign_Transfers) {
    auto& bus = ayt::event::EventBus::instance();
    ayt::app::EventBusHostScope a;
    ayt::app::EventBusHostScope b;

    a.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});
    a.subscribe<HostScopeTestEvent>([](const HostScopeTestEvent&) {});

    b = std::move(a);
    CHECK_TRUE(a.empty());
    CHECK_INT_EQ(static_cast<int>(b.size()), 2);

    b.disconnect();
    (void)bus; // suppress unused warning
}

TEST_SUITE_END

} // namespace ayt::app::test

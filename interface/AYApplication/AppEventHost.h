#pragma once
// ABI-compatible facade for callers that used the former Application-owned
// subscription scope. New code should include
// <AYEventSystem/SubscriptionScope.h> and use ayt::event::SubscriptionScope.

#include <AYEventSystem/SubscriptionScope.h>

namespace ayt::app
{

class EventBusHostScope {
public:
    EventBusHostScope() = default;
    EventBusHostScope(EventBusHostScope&& other) noexcept;
    EventBusHostScope& operator=(EventBusHostScope&& other) noexcept;
    EventBusHostScope(const EventBusHostScope&) = delete;
    EventBusHostScope& operator=(const EventBusHostScope&) = delete;
    ~EventBusHostScope() = default;

    template <typename EventType>
    ayt::event::ConnectionId subscribe(
        std::function<void(const EventType&)> listener)
    {
        return _scope.subscribe<EventType>(std::move(listener));
    }

    void disconnect();

    [[nodiscard]] std::size_t size() const noexcept { return _scope.size(); }
    [[nodiscard]] bool empty() const noexcept { return _scope.empty(); }
    [[nodiscard]] EventBusHostScope detach();

private:
    // Keep this as the sole member. SubscriptionScope intentionally retains
    // the former vector-only representation so existing Application clients
    // keep the same object size, alignment, and member offset.
    ayt::event::SubscriptionScope _scope;
};

static_assert(sizeof(EventBusHostScope) ==
              sizeof(ayt::event::SubscriptionScope));
static_assert(alignof(EventBusHostScope) ==
              alignof(ayt::event::SubscriptionScope));

} // namespace ayt::app

#include <AYApplication/AppEventHost.h>

namespace ayt::app
{

EventBusHostScope::EventBusHostScope(EventBusHostScope&& other) noexcept
    : _scope(std::move(other._scope))
{
}

EventBusHostScope& EventBusHostScope::operator=(
    EventBusHostScope&& other) noexcept
{
    if (this != &other) {
        _scope = std::move(other._scope);
    }
    return *this;
}

void EventBusHostScope::disconnect()
{
    _scope.disconnect();
}

EventBusHostScope EventBusHostScope::detach()
{
    EventBusHostScope child;
    child._scope = _scope.detach();
    return child;
}

} // namespace ayt::app

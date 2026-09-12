#include <AYApplication/GameFlowDiagnostics.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ayt::app
{

GameFlowDiagnostics::GameFlowDiagnostics(std::size_t historyCapacity)
    : _historyCapacity(historyCapacity)
{
}

void GameFlowDiagnostics::setHistoryCapacity(std::size_t capacity) noexcept
{
    _historyCapacity = capacity;
    while (_events.size() > _historyCapacity) _events.pop_front();
    const std::size_t pendingCapacity = observerQueueCapacity();
    while (_pendingObserverEvents.size() > pendingCapacity) {
        _pendingObserverEvents.pop_front();
        ++_metrics.observerEventsDropped;
    }
}

std::size_t GameFlowDiagnostics::historyCapacity() const noexcept
{
    return _historyCapacity;
}

const std::deque<GameFlowEvent>& GameFlowDiagnostics::events() const noexcept
{
    return _events;
}

void GameFlowDiagnostics::clearEvents() noexcept
{
    _events.clear();
}

const GameFlowMetrics& GameFlowDiagnostics::metrics() const noexcept
{
    return _metrics;
}

void GameFlowDiagnostics::resetMetrics() noexcept
{
    _metrics = {};
}

void GameFlowDiagnostics::setObserver(GameFlowEventObserver observer) noexcept
{
    try {
        _observer = std::move(observer);
        _pendingObserverEvents.clear();
    } catch (...) {
        _observer = {};
        _pendingObserverEvents.clear();
        ++_metrics.observerFailures;
    }
}

void GameFlowDiagnostics::record(GameFlowEvent event) noexcept
{
    try {
        event.serial = _nextSerial++;
        updateMetrics(event.kind);
        if (_historyCapacity > 0u) {
            _events.push_back(event);
            while (_events.size() > _historyCapacity) _events.pop_front();
        }
        if (_observer) {
            const std::size_t capacity = observerQueueCapacity();
            if (_pendingObserverEvents.size() >= capacity) {
                _pendingObserverEvents.pop_front();
                ++_metrics.observerEventsDropped;
            }
            _pendingObserverEvents.push_back(std::move(event));
        }
    } catch (...) {
        // Diagnostics must never alter flow execution. A failed allocation may
        // create a serial gap, which is useful evidence that capture degraded.
        ++_metrics.eventsDropped;
    }
}

void GameFlowDiagnostics::flushObserver() noexcept
{
    if (_dispatchingObserver || !_observer
        || _pendingObserverEvents.empty()) return;

    _dispatchingObserver = true;
    _observerDispatchBatch.swap(_pendingObserverEvents);
    while (!_observerDispatchBatch.empty()) {
        GameFlowEvent event;
        try {
            event = std::move(_observerDispatchBatch.front());
        } catch (...) {
            _observerDispatchBatch.pop_front();
            ++_metrics.eventsDropped;
            continue;
        }
        _observerDispatchBatch.pop_front();
        try {
            const auto observer = _observer;
            if (observer) observer(event);
        } catch (...) {
            ++_metrics.observerFailures;
        }
    }
    _dispatchingObserver = false;
}

void GameFlowDiagnostics::recordUpdate(
    double deltaSeconds, std::uint64_t wallTimeNanoseconds) noexcept
{
    ++_metrics.updateCalls;
    if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0) {
        _metrics.accumulatedDeltaSeconds += deltaSeconds;
    }
    _metrics.accumulatedUpdateWallTimeNanoseconds += wallTimeNanoseconds;
}

void GameFlowDiagnostics::observeQueueDepth(std::size_t count) noexcept
{
    _metrics.maxQueuedIntentCount = std::max(
        _metrics.maxQueuedIntentCount, count);
}

void GameFlowDiagnostics::observeCallDepth(std::size_t depth) noexcept
{
    _metrics.maxCallDepth = std::max(_metrics.maxCallDepth, depth);
}

GameFlowDiagnosticsSnapshot GameFlowDiagnostics::snapshot(
    GameFlowRuntimeDiagnosticState runtime) const
{
    GameFlowDiagnosticsSnapshot result;
    result.runtime = std::move(runtime);
    result.metrics = _metrics;
    result.recentEvents.assign(_events.begin(), _events.end());
    return result;
}

void GameFlowDiagnostics::updateMetrics(GameFlowEventKind kind) noexcept
{
    switch (kind) {
    case GameFlowEventKind::IntentQueued:
        ++_metrics.intentsRequested;
        ++_metrics.intentsQueued;
        break;
    case GameFlowEventKind::IntentRejected:
        ++_metrics.intentsRequested;
        ++_metrics.intentsRejected;
        break;
    case GameFlowEventKind::IntentUnmatched:
        ++_metrics.intentsUnmatched;
        break;
    case GameFlowEventKind::GuardAccepted:
        ++_metrics.guardsAccepted;
        break;
    case GameFlowEventKind::GuardRejected:
        ++_metrics.guardsRejected;
        break;
    case GameFlowEventKind::GuardFailed:
        ++_metrics.guardsFailed;
        break;
    case GameFlowEventKind::TransitionStarted:
        ++_metrics.transitionsStarted;
        break;
    case GameFlowEventKind::TransitionSucceeded:
        ++_metrics.transitionsSucceeded;
        break;
    case GameFlowEventKind::TransitionFailed:
        ++_metrics.transitionsFailed;
        break;
    case GameFlowEventKind::TransitionCancelled:
        ++_metrics.transitionsCancelled;
        break;
    case GameFlowEventKind::TransitionTimedOut:
        ++_metrics.transitionsFailed;
        ++_metrics.transitionsTimedOut;
        break;
    case GameFlowEventKind::ActionStarted:
        ++_metrics.actionsStarted;
        break;
    case GameFlowEventKind::ActionPending:
        ++_metrics.actionsPending;
        break;
    case GameFlowEventKind::ActionSucceeded:
        ++_metrics.actionsSucceeded;
        break;
    case GameFlowEventKind::ActionFailed:
        ++_metrics.actionsFailed;
        break;
    case GameFlowEventKind::ActionCancelled:
        ++_metrics.actionsCancelled;
        break;
    case GameFlowEventKind::ActionCompletionRejected:
        ++_metrics.rejectedActionCompletions;
        break;
    case GameFlowEventKind::SubflowEntered:
        ++_metrics.subflowsEntered;
        break;
    case GameFlowEventKind::SubflowReturned:
        ++_metrics.subflowsReturned;
        break;
    case GameFlowEventKind::SubflowCancelled:
        ++_metrics.subflowsCancelled;
        break;
    default:
        break;
    }
}

std::size_t GameFlowDiagnostics::observerQueueCapacity() const noexcept
{
    return std::max<std::size_t>(_historyCapacity, 1u);
}

} // namespace ayt::app

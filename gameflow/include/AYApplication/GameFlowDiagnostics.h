#pragma once

#include <AYApplication/GameFlowActionRegistry.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

inline constexpr std::uint32_t kGameFlowDiagnosticsSchemaVersion = 1u;

// Values are explicit because events may be consumed by tooling, telemetry,
// and crash-report serializers built independently from the runtime.
enum class GameFlowEventKind : std::uint16_t
{
    ProgramInitialized = 1,
    ProgramReloaded = 2,
    CoordinatorReset = 3,
    ConfigurationRejected = 4,
    StateRestored = 5,

    IntentQueued = 100,
    IntentRejected = 101,
    IntentUnmatched = 102,

    GuardAccepted = 200,
    GuardRejected = 201,
    GuardFailed = 202,

    TransitionStarted = 300,
    TransitionSucceeded = 301,
    TransitionFailed = 302,
    TransitionCancelled = 303,
    TransitionTimedOut = 304,

    ActionStarted = 400,
    ActionPending = 401,
    ActionSucceeded = 402,
    ActionFailed = 403,
    ActionCancelled = 404,
    ActionCompletionRejected = 405,

    SubflowEntered = 500,
    SubflowReturned = 501,
    SubflowCancelled = 502,
};

enum class GameFlowEventReason : std::uint16_t
{
    None = 0,
    NotReady = 1,
    UnknownIntent = 2,
    InvalidPayload = 3,
    NoMatchingTransition = 4,
    MissingHandler = 5,
    HandlerException = 6,
    InvalidAsyncResult = 7,
    Timeout = 8,
    ExplicitCancellation = 9,
    SubflowUnavailable = 10,
    CallDepthLimit = 11,
    RecursiveCall = 12,
    InvalidContract = 13,
    RootReturn = 14,
    MissingCaller = 15,
    StaleCompletion = 16,
    UnsafeReloadPoint = 17,
    InvalidConfiguration = 18,
    RuntimeReset = 19,
};

// This record deliberately has no payload, action arguments, exception text,
// or user-provided message field. Stable schema identifiers are sufficient to
// correlate a failure without copying game or player data into diagnostics.
struct GameFlowEvent
{
    std::uint64_t serial = 0;
    GameFlowEventKind kind = GameFlowEventKind::CoordinatorReset;
    GameFlowEventReason reason = GameFlowEventReason::None;
    GameFlowGeneration generation = 0;
    GameFlowActionExecutionId actionExecutionId = 0;
    std::string flowId;
    std::string stateId;
    std::string transitionId;
    std::string intentId;
    std::string actionId;
    std::string guardId;
    std::size_t callDepth = 0;
};

struct GameFlowMetrics
{
    std::uint64_t updateCalls = 0;
    double accumulatedDeltaSeconds = 0.0;
    std::uint64_t accumulatedUpdateWallTimeNanoseconds = 0;

    std::uint64_t intentsRequested = 0;
    std::uint64_t intentsQueued = 0;
    std::uint64_t intentsRejected = 0;
    std::uint64_t intentsUnmatched = 0;

    std::uint64_t guardsAccepted = 0;
    std::uint64_t guardsRejected = 0;
    std::uint64_t guardsFailed = 0;

    std::uint64_t transitionsStarted = 0;
    std::uint64_t transitionsSucceeded = 0;
    std::uint64_t transitionsFailed = 0;
    std::uint64_t transitionsCancelled = 0;
    std::uint64_t transitionsTimedOut = 0;

    std::uint64_t actionsStarted = 0;
    std::uint64_t actionsPending = 0;
    std::uint64_t actionsSucceeded = 0;
    std::uint64_t actionsFailed = 0;
    std::uint64_t actionsCancelled = 0;
    std::uint64_t rejectedActionCompletions = 0;

    std::uint64_t subflowsEntered = 0;
    std::uint64_t subflowsReturned = 0;
    std::uint64_t subflowsCancelled = 0;

    std::uint64_t observerFailures = 0;
    std::uint64_t eventsDropped = 0;
    std::uint64_t observerEventsDropped = 0;
    std::size_t maxQueuedIntentCount = 0;
    std::size_t maxCallDepth = 0;
};

struct GameFlowDiagnosticFrame
{
    std::uint64_t instanceSerial = 0;
    std::string flowId;
    std::string stateId;
    std::string suspendedTransitionId;
};

struct GameFlowRuntimeDiagnosticState
{
    bool ready = false;
    bool busy = false;
    bool waitingForAction = false;
    bool waitingForSubflow = false;
    std::string currentFlowId;
    std::string currentStateId;
    std::string activeTransitionId;
    std::string activeActionId;
    GameFlowGeneration generation = 0;
    GameFlowActionExecutionId actionExecutionId = 0;
    std::size_t queuedIntentCount = 0;
    std::size_t callDepth = 0;
    std::vector<GameFlowDiagnosticFrame> frames;
};

// Fully owning and copyable so a crash reporter can retain it after the
// coordinator is reset or destroyed.
struct GameFlowDiagnosticsSnapshot
{
    std::uint32_t schemaVersion = kGameFlowDiagnosticsSchemaVersion;
    GameFlowRuntimeDiagnosticState runtime;
    GameFlowMetrics metrics;
    std::vector<GameFlowEvent> recentEvents;
};

using GameFlowEventObserver = std::function<void(const GameFlowEvent&)>;

class GameFlowDiagnostics
{
public:
    static constexpr std::size_t kDefaultHistoryCapacity = 256u;

    explicit GameFlowDiagnostics(
        std::size_t historyCapacity = kDefaultHistoryCapacity);

    void setHistoryCapacity(std::size_t capacity) noexcept;
    [[nodiscard]] std::size_t historyCapacity() const noexcept;
    [[nodiscard]] const std::deque<GameFlowEvent>& events() const noexcept;
    void clearEvents() noexcept;

    [[nodiscard]] const GameFlowMetrics& metrics() const noexcept;
    void resetMetrics() noexcept;

    // Notification is deferred until flushObserver(). Reentrant records are
    // retained and delivered on a later flush; observer exceptions never leave
    // the diagnostics boundary.
    void setObserver(GameFlowEventObserver observer) noexcept;
    void record(GameFlowEvent event) noexcept;
    void flushObserver() noexcept;

    void recordUpdate(double deltaSeconds,
                      std::uint64_t wallTimeNanoseconds) noexcept;
    void observeQueueDepth(std::size_t count) noexcept;
    void observeCallDepth(std::size_t depth) noexcept;

    [[nodiscard]] GameFlowDiagnosticsSnapshot snapshot(
        GameFlowRuntimeDiagnosticState runtime) const;

private:
    void updateMetrics(GameFlowEventKind kind) noexcept;
    [[nodiscard]] std::size_t observerQueueCapacity() const noexcept;

    std::size_t _historyCapacity = kDefaultHistoryCapacity;
    std::uint64_t _nextSerial = 1;
    std::deque<GameFlowEvent> _events;
    GameFlowMetrics _metrics;
    GameFlowEventObserver _observer;
    std::deque<GameFlowEvent> _pendingObserverEvents;
    std::deque<GameFlowEvent> _observerDispatchBatch;
    bool _dispatchingObserver = false;
};

} // namespace ayt::app

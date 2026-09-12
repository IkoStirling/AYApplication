#pragma once

#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/GameFlowDiagnostics.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

struct GameFlowProgram;

struct GameFlowNormalizedTransition
{
    std::size_t documentIndex = 0;
    std::size_t fromState = 0;
    std::size_t toState = 0;
    std::size_t intent = 0;
    std::size_t onFailureState = static_cast<std::size_t>(-1);
    std::size_t onCancelState = static_cast<std::size_t>(-1);
};

// Runtime plan owns a copy of the validated document. Lookup tables are built
// once, so frame execution never depends on editor node positions or JSON order
// beyond the documented priority/document-order tie-break.
struct GameFlowPlan
{
    GameFlowDocument document;
    std::map<std::string, std::size_t, std::less<>> stateIndices;
    std::map<std::string, std::size_t, std::less<>> intentIndices;
    std::vector<GameFlowNormalizedTransition> transitions;
    std::map<std::string, std::vector<std::size_t>, std::less<>>
        transitionsByStateAndIntent;
};

[[nodiscard]] bool buildGameFlowPlan(
    const GameFlowDocument& document,
    const GameFlowActionRegistry& registry,
    GameFlowPlan& plan,
    std::vector<GameFlowDiagnostic>* diagnostics = nullptr);

enum class GameFlowRequestState : std::uint8_t
{
    Queued,
    UnknownIntent,
    InvalidPayload,
    NotReady,
};

struct GameFlowRequestResult
{
    GameFlowRequestState state = GameFlowRequestState::NotReady;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return state == GameFlowRequestState::Queued;
    }
};

struct GameFlowTraceEntry
{
    std::uint64_t serial = 0;
    GameFlowGeneration generation = 0;
    GameFlowActionExecutionId actionExecutionId = 0;
    std::string state;
    std::string transition;
    std::string detail;
    std::string flowId;
    std::size_t callDepth = 0;
};

inline constexpr std::size_t kNoGameFlowActionIndex =
    static_cast<std::size_t>(-1);

enum class GameFlowCoordinatorStatus : std::uint8_t
{
    NotReady,
    Idle,
    Queued,
    Running,
    WaitingForAction,
    WaitingForSubflow,
};

struct GameFlowStackFrameSnapshot
{
    std::uint64_t instanceSerial = 0;
    std::string flowId;
    std::string stateId;
    std::string suspendedTransitionId;
};

// An owning, point-in-time view intended for tooling and diagnostics. The
// strings remain valid even if the coordinator advances or changes plan after
// the snapshot is taken.
struct GameFlowCoordinatorSnapshot
{
    GameFlowCoordinatorStatus status = GameFlowCoordinatorStatus::NotReady;
    std::string currentStateId;
    std::string activeTransitionId;
    std::string activeActionId;
    std::size_t activeActionIndex = kNoGameFlowActionIndex;
    GameFlowGeneration generation = 0;
    GameFlowActionExecutionId executionId = 0;
    std::size_t queuedIntentCount = 0;
    bool busy = false;
    std::string currentFlowId;
    std::string qualifiedStateId;
    std::size_t callDepth = 0;
    std::vector<GameFlowStackFrameSnapshot> frames;
};

class GameFlowCoordinator
{
public:
    GameFlowCoordinator();
    ~GameFlowCoordinator();

    GameFlowCoordinator(const GameFlowCoordinator&) = delete;
    GameFlowCoordinator& operator=(const GameFlowCoordinator&) = delete;
    GameFlowCoordinator(GameFlowCoordinator&&) noexcept;
    GameFlowCoordinator& operator=(GameFlowCoordinator&&) noexcept;

    // plan and registry must outlive the coordinator or the next setPlan().
    bool setPlan(
        const GameFlowPlan* plan,
        const GameFlowActionRegistry* registry,
        std::string* error = nullptr);
    bool setProgram(
        const GameFlowProgram* program,
        const GameFlowActionRegistry* registry,
        GameFlowPayload rootParameters = {},
        std::string* error = nullptr);
    // Atomically replaces a complete program at a reload safe point. The
    // current root state is preserved when the new root contains it;
    // otherwise the new root starts from its declared initial state.
    bool replaceProgram(
        const GameFlowProgram* program,
        const GameFlowActionRegistry* registry,
        GameFlowPayload rootParameters = {},
        std::string* error = nullptr);
    void reset() noexcept;

    GameFlowRequestResult request(
        std::string_view intent,
        GameFlowPayload payload = {});

    // Advances queued work and timeout accounting. Non-finite or negative
    // deltas are treated as zero.
    void update(double deltaSeconds = 0.0);

    bool completeAction(
        GameFlowActionExecutionId executionId,
        GameFlowActionResult result,
        std::string* error = nullptr);
    bool cancelActive(std::string message = {});
    bool cancelSubflowCall(std::string message = {});

    [[nodiscard]] std::string_view currentState() const noexcept;
    [[nodiscard]] std::string_view currentFlow() const noexcept;
    [[nodiscard]] std::string qualifiedState() const;
    [[nodiscard]] std::size_t callDepth() const noexcept;
    [[nodiscard]] std::string_view activeTransition() const noexcept;
    [[nodiscard]] GameFlowGeneration activeGeneration() const noexcept;
    [[nodiscard]] GameFlowActionExecutionId pendingAction() const noexcept;
    [[nodiscard]] std::size_t queuedIntentCount() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    // Hot reload may swap plans only when no transition, queued intent, or
    // nested subflow can still reference the old program.
    [[nodiscard]] bool reloadSafePoint() const noexcept;
    bool restoreState(std::string_view flowId,
                      std::string_view stateId,
                      std::string* error = nullptr);
    [[nodiscard]] GameFlowCoordinatorSnapshot snapshot() const;

    [[nodiscard]] const std::vector<GameFlowTraceEntry>& trace() const noexcept;
    void clearTrace() noexcept;

    void setEventHistoryCapacity(std::size_t capacity) noexcept;
    [[nodiscard]] std::size_t eventHistoryCapacity() const noexcept;
    [[nodiscard]] const std::deque<GameFlowEvent>& eventHistory() const noexcept;
    void clearEventHistory() noexcept;
    [[nodiscard]] const GameFlowMetrics& metrics() const noexcept;
    void resetMetrics() noexcept;
    void setEventObserver(GameFlowEventObserver observer) noexcept;
    [[nodiscard]] GameFlowDiagnosticsSnapshot diagnosticsSnapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app

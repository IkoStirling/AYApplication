#pragma once

#include <AYApplication/GameFlowActionRegistry.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

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

    [[nodiscard]] std::string_view currentState() const noexcept;
    [[nodiscard]] std::string_view activeTransition() const noexcept;
    [[nodiscard]] GameFlowGeneration activeGeneration() const noexcept;
    [[nodiscard]] GameFlowActionExecutionId pendingAction() const noexcept;
    [[nodiscard]] std::size_t queuedIntentCount() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] GameFlowCoordinatorSnapshot snapshot() const;

    [[nodiscard]] const std::vector<GameFlowTraceEntry>& trace() const noexcept;
    void clearTrace() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app

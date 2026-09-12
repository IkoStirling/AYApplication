#include <AYApplication/GameFlowCoordinator.h>
#include <AYTest.h>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{

using namespace ayt::app;

GameFlowDocument makeDiagnosticFlow(std::string action = {})
{
    GameFlowDocument document;
    document.id = "diagnostic-flow";
    document.initialState = "idle";
    document.intents = {{"go", {
        {"token", GameFlowValueType::String, true, {}},
    }}};
    document.states = {{"idle"}, {"done"}, {"failed"}, {"cancelled"}};

    GameFlowTransitionDefinition transition;
    transition.id = "start";
    transition.fromState = "idle";
    transition.triggerIntent = "go";
    transition.toState = "done";
    if (!action.empty()) transition.actions.push_back({std::move(action), {}});
    transition.onFailureState = "failed";
    transition.onCancelState = "cancelled";
    document.transitions.push_back(std::move(transition));
    return document;
}

bool containsText(const GameFlowEvent& event, std::string_view needle)
{
    return event.flowId.find(needle) != std::string::npos
        || event.stateId.find(needle) != std::string::npos
        || event.transitionId.find(needle) != std::string::npos
        || event.intentId.find(needle) != std::string::npos
        || event.actionId.find(needle) != std::string::npos
        || event.guardId.find(needle) != std::string::npos;
}

} // namespace

TEST_SUITE(GameFlowDiagnosticsTests)

TEST_CASE(structured_history_is_bounded_and_excludes_runtime_values)
{
    constexpr std::string_view secret = "player-secret-8e72";
    GameFlowActionRegistry registry;
    CHECK(registry.registerAction({"test.fail", {}, false},
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::failed(
                "player-secret-8e72 from host exception");
        }));
    const GameFlowDocument document = makeDiagnosticFlow("test.fail");
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));

    GameFlowCoordinator coordinator;
    coordinator.setEventHistoryCapacity(5u);
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("go", {{"token", std::string(secret)}}));
    coordinator.update(0.25);

    CHECK(coordinator.currentState() == "failed");
    CHECK(coordinator.eventHistory().size() == 5u);
    std::uint64_t previousSerial = 0;
    for (const auto& event : coordinator.eventHistory()) {
        CHECK(event.serial > previousSerial);
        CHECK_FALSE(containsText(event, secret));
        previousSerial = event.serial;
    }

    const auto& metrics = coordinator.metrics();
    CHECK(metrics.updateCalls == 1u);
    CHECK(metrics.accumulatedDeltaSeconds == 0.25);
    CHECK(metrics.intentsRequested == 1u);
    CHECK(metrics.intentsQueued == 1u);
    CHECK(metrics.transitionsStarted == 1u);
    CHECK(metrics.transitionsFailed == 1u);
    CHECK(metrics.actionsStarted == 1u);
    CHECK(metrics.actionsFailed == 1u);
    CHECK(metrics.maxQueuedIntentCount == 1u);
}

TEST_CASE(observer_exceptions_are_isolated_and_reentrant_requests_are_safe)
{
    GameFlowActionRegistry registry;
    const GameFlowDocument document = makeDiagnosticFlow();
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));

    bool reentered = false;
    bool reentrantRequestAccepted = false;
    std::size_t observed = 0;
    std::size_t observedQueued = 0;
    coordinator.setEventObserver([&](const GameFlowEvent& event) {
        ++observed;
        if (event.kind == GameFlowEventKind::IntentQueued) ++observedQueued;
        if (event.kind == GameFlowEventKind::IntentQueued && !reentered) {
            reentered = true;
            reentrantRequestAccepted = static_cast<bool>(
                coordinator.request("go", {{"token", "nested"}}));
            throw std::runtime_error("observer failure");
        }
    });

    CHECK(coordinator.request("go", {{"token", "outer"}}));
    CHECK(reentered);
    CHECK(reentrantRequestAccepted);
    CHECK(coordinator.queuedIntentCount() == 2u);
    CHECK(coordinator.metrics().observerFailures == 1u);
    CHECK(observedQueued == 1u);

    coordinator.update();
    CHECK(coordinator.currentState() == "done");
    CHECK(coordinator.queuedIntentCount() == 0u);
    CHECK(coordinator.metrics().intentsQueued == 2u);
    CHECK(coordinator.metrics().intentsUnmatched == 1u);
    CHECK(observed >= 2u);
    CHECK(observedQueued == 2u);
}

TEST_CASE(crash_snapshot_owns_pending_state_metrics_and_sanitized_events)
{
    static_assert(std::is_copy_constructible_v<GameFlowDiagnosticsSnapshot>);
    constexpr std::string_view secret = "session-key-44";
    GameFlowActionRegistry registry;
    CHECK(registry.registerAction({"test.pending", {}, true},
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::pending();
        }));
    const GameFlowDocument document = makeDiagnosticFlow("test.pending");
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("go", {{"token", std::string(secret)}}));
    coordinator.update();

    const GameFlowDiagnosticsSnapshot captured =
        coordinator.diagnosticsSnapshot();
    CHECK(captured.schemaVersion == kGameFlowDiagnosticsSchemaVersion);
    CHECK(captured.runtime.ready);
    CHECK(captured.runtime.busy);
    CHECK(captured.runtime.waitingForAction);
    CHECK(captured.runtime.currentFlowId == "diagnostic-flow");
    CHECK(captured.runtime.currentStateId == "idle");
    CHECK(captured.runtime.activeTransitionId == "start");
    CHECK(captured.runtime.activeActionId == "test.pending");
    CHECK(captured.runtime.actionExecutionId != 0u);
    CHECK(captured.runtime.frames.size() == 1u);
    CHECK(captured.runtime.frames[0].flowId == "diagnostic-flow");
    CHECK(captured.runtime.frames[0].stateId == "idle");
    CHECK(captured.metrics.actionsPending == 1u);
    CHECK_FALSE(captured.recentEvents.empty());
    for (const auto& event : captured.recentEvents) {
        CHECK_FALSE(containsText(event, secret));
    }

    const std::size_t capturedEventCount = captured.recentEvents.size();
    coordinator.clearEventHistory();
    coordinator.resetMetrics();
    coordinator.reset();
    CHECK(coordinator.metrics().actionsCancelled == 1u);
    CHECK(captured.recentEvents.size() == capturedEventCount);
    CHECK(captured.metrics.actionsPending == 1u);
    CHECK(captured.runtime.waitingForAction);
}

TEST_SUITE_END

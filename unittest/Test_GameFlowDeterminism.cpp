#include <AYApplication/GameFlowCoordinator.h>
#include <AYApplication/GameFlowProgram.h>
#include <AYTest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using namespace ayt::app;

class MemoryExchange final : public IGameFlowDeterminismExchange
{
public:
    explicit MemoryExchange(GameFlowDeterminismMode valueMode)
        : value(valueMode)
    {
    }

    GameFlowDeterminismMode mode() const noexcept override { return value; }

    bool exchange(GameFlowDeterminismRecord& record,
                  std::string& error) override
    {
        if (!storedFault.empty()) {
            error = storedFault;
            return false;
        }
        try {
            if (value == GameFlowDeterminismMode::Capture) {
                records.push_back(record);
            } else if (position < records.size()) {
                record = records[position++];
            } else {
                storedFault = "memory replay ended";
            }
        } catch (...) {
            storedFault = "memory replay failed";
        }
        error = storedFault;
        return storedFault.empty();
    }

    bool healthy() const noexcept override { return storedFault.empty(); }
    std::string_view fault() const noexcept override { return storedFault; }

    GameFlowDeterminismMode value;
    std::vector<GameFlowDeterminismRecord> records;
    std::size_t position = 0u;
    std::string storedFault;
};

GameFlowActionCall control(
    std::string id, GameFlowPayload arguments = {})
{
    return {std::move(id), std::move(arguments)};
}

struct DeterminismFixture
{
    GameFlowActionRegistry registry;
    GameFlowDocument root;
    GameFlowDocument child;
    GameFlowProgram program;
    int guardCalls = 0;
    int pendingCalls = 0;
    int parentCalls = 0;
    int cancellationCalls = 0;

    DeterminismFixture()
    {
        CHECK(registry.registerGuard({"test.allow", {}},
            [this](const GameFlowGuardInvocation&) {
                ++guardCalls;
                return true;
            }));
        CHECK(registry.registerAction({"test.pending", {}, true},
            [this](const GameFlowActionInvocation&) {
                ++pendingCalls;
                return GameFlowActionResult::pending([this]() {
                    ++cancellationCalls;
                });
            }));
        CHECK(registry.registerAction({"test.parent", {}, false},
            [this](const GameFlowActionInvocation&) {
                ++parentCalls;
                return GameFlowActionResult::succeeded();
            }));

        root.id = "root";
        root.initialState = "menu";
        root.intents = {{"start", {
            {"profile", GameFlowValueType::String, true, {}},
        }}};
        root.states = {{"menu"}, {"done"}, {"failed"}, {"cancelled"}};
        GameFlowTransitionDefinition start;
        start.id = "start_child";
        start.fromState = "menu";
        start.triggerIntent = "start";
        start.toState = "done";
        start.actions = {
            control(std::string(kGameFlowActionEnter), {
                {std::string(kGameFlowSubflowIdArgument), "child"},
            }),
            {"test.parent", {}},
        };
        start.onFailureState = "failed";
        start.onCancelState = "cancelled";
        root.transitions.push_back(std::move(start));

        child.id = "child";
        child.initialState = "waiting";
        child.entryParameters = {
            {"profile", GameFlowValueType::String, true, {}},
        };
        child.result = {
            {"score", GameFlowValueType::Integer, true, {}},
        };
        child.intents = {{"finish", {
            {"score", GameFlowValueType::Integer, true, {}},
        }}};
        child.states = {{"waiting"}, {"returned"}, {"failed"},
            {"cancelled"}};
        GameFlowTransitionDefinition finish;
        finish.id = "finish_child";
        finish.fromState = "waiting";
        finish.triggerIntent = "finish";
        finish.toState = "returned";
        finish.guard = {"test.allow", {}};
        finish.actions = {
            {"test.pending", {}},
            control(std::string(kGameFlowActionReturn)),
        };
        finish.onFailureState = "failed";
        finish.onCancelState = "cancelled";
        child.transitions.push_back(std::move(finish));

        CHECK(buildGameFlowProgram(root, registry,
            [this](std::string_view id, GameFlowDocument& document,
                   std::string&) {
                if (id != child.id) return false;
                document = child;
                return true;
            }, program));
    }
};

bool hasKind(const std::vector<GameFlowDeterminismRecord>& values,
             GameFlowDeterminismKind kind)
{
    return std::any_of(values.begin(), values.end(),
        [kind](const auto& value) { return value.kind == kind; });
}

struct ReentrantFixture
{
    GameFlowActionRegistry registry;
    GameFlowPlan plan;
    GameFlowCoordinator* coordinator = nullptr;
    int guardCalls = 0;
    int actionCalls = 0;
    int cancellationCalls = 0;

    ReentrantFixture()
    {
        CHECK(registry.registerGuard({"test.reentrant-guard", {}},
            [this](const GameFlowGuardInvocation&) {
                ++guardCalls;
                CHECK(coordinator != nullptr);
                CHECK(coordinator->request("fromGuard"));
                return true;
            }));
        CHECK(registry.registerAction(
            {"test.reentrant-action", {}, true},
            [this](const GameFlowActionInvocation&) {
                ++actionCalls;
                CHECK(coordinator != nullptr);
                CHECK(coordinator->request("fromAction"));
                return GameFlowActionResult::pending([this]() {
                    ++cancellationCalls;
                    CHECK(coordinator != nullptr);
                    CHECK(coordinator->request("fromCancellation"));
                });
            }));

        GameFlowDocument document;
        document.id = "reentrant";
        document.initialState = "idle";
        document.intents = {{"go"}, {"fromGuard"}, {"fromAction"},
            {"fromCancellation"}};
        document.states = {{"idle"}, {"done"}, {"cancelled"},
            {"recovered"}};

        GameFlowTransitionDefinition go;
        go.id = "go";
        go.fromState = "idle";
        go.triggerIntent = "go";
        go.toState = "done";
        go.guard.guard = "test.reentrant-guard";
        go.actions = {{"test.reentrant-action", {}}};
        go.onCancelState = "cancelled";
        document.transitions.push_back(std::move(go));

        for (const auto& [intent, target] : std::vector<
                 std::pair<std::string, std::string>>{
                 {"fromGuard", "cancelled"},
                 {"fromAction", "cancelled"},
                 {"fromCancellation", "recovered"}}) {
            GameFlowTransitionDefinition followup;
            followup.id = intent + "-transition";
            followup.fromState = "cancelled";
            followup.triggerIntent = intent;
            followup.toState = target;
            document.transitions.push_back(std::move(followup));
        }
        CHECK(buildGameFlowPlan(document, registry, plan));
    }
};

} // namespace

TEST_SUITE(GameFlowDeterminismTests)

TEST_CASE(capture_and_playback_inject_guard_action_async_and_subflow_results)
{
    DeterminismFixture fixture;
    MemoryExchange capture(GameFlowDeterminismMode::Capture);
    GameFlowCoordinator live;
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setProgram(&fixture.program, &fixture.registry));
    CHECK(live.request("start", {{"profile", "p1"}}));
    live.update(0.125);
    CHECK(live.currentFlow() == "child");
    CHECK(live.request("finish", {{"score", std::int64_t(17)}}));
    live.update(0.25);
    const auto execution = live.pendingAction();
    CHECK(execution != 0u);
    CHECK(live.completeAction(execution, GameFlowActionResult::succeeded()));
    CHECK(live.currentFlow() == "root");
    CHECK(live.currentState() == "done");
    CHECK(fixture.guardCalls == 1);
    CHECK(fixture.pendingCalls == 1);
    CHECK(fixture.parentCalls == 1);
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::Intent));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::Update));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::GuardOutcome));
    CHECK(hasKind(capture.records,
        GameFlowDeterminismKind::TransitionDecision));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::ActionOutcome));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::AsyncCompletion));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::SubflowEnter));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::SubflowReturn));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::Terminal));

    fixture.guardCalls = 0;
    fixture.pendingCalls = 0;
    fixture.parentCalls = 0;
    MemoryExchange playback(GameFlowDeterminismMode::Playback);
    playback.records = capture.records;
    GameFlowActionRegistry playbackRegistry;
    CHECK(playbackRegistry.registerGuardType({"test.allow", {}}));
    CHECK(playbackRegistry.registerActionType(
        {"test.pending", {}, true}));
    CHECK(playbackRegistry.registerActionType(
        {"test.parent", {}, false}));
    GameFlowCoordinator replayed;
    CHECK(replayed.setDeterminismExchange(&playback));
    CHECK(replayed.setProgram(&fixture.program, &playbackRegistry));
    CHECK(replayed.request("start", {{"profile", "p1"}}));
    replayed.update(0.125);
    CHECK(replayed.request("finish", {{"score", std::int64_t(17)}}));
    replayed.update(0.25);
    CHECK(replayed.completeAction(replayed.pendingAction(),
        GameFlowActionResult::failed("ignored live outcome")));
    CHECK(replayed.currentFlow() == "root");
    CHECK(replayed.currentState() == "done");
    CHECK(replayed.determinismHealthy());
    CHECK(playback.position == playback.records.size());
    CHECK(fixture.guardCalls == 0);
    CHECK(fixture.pendingCalls == 0);
    CHECK(fixture.parentCalls == 0);
}

TEST_CASE(cancellation_is_captured_and_playback_skips_live_cancel_callback)
{
    DeterminismFixture fixture;
    MemoryExchange capture(GameFlowDeterminismMode::Capture);
    GameFlowCoordinator live;
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setProgram(&fixture.program, &fixture.registry));
    CHECK(live.request("start", {{"profile", "p2"}}));
    live.update();
    CHECK(live.request("finish", {{"score", std::int64_t(3)}}));
    live.update();
    CHECK(live.pendingAction() != 0u);
    CHECK(live.cancelSubflowCall("user left"));
    CHECK(live.currentFlow() == "root");
    CHECK(live.currentState() == "cancelled");
    CHECK(fixture.cancellationCalls == 1);
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::Cancellation));
    CHECK(hasKind(capture.records, GameFlowDeterminismKind::Terminal));

    fixture.guardCalls = 0;
    fixture.pendingCalls = 0;
    fixture.cancellationCalls = 0;
    MemoryExchange playback(GameFlowDeterminismMode::Playback);
    playback.records = capture.records;
    GameFlowActionRegistry playbackRegistry;
    CHECK(playbackRegistry.registerGuardType({"test.allow", {}}));
    CHECK(playbackRegistry.registerActionType(
        {"test.pending", {}, true}));
    CHECK(playbackRegistry.registerActionType(
        {"test.parent", {}, false}));
    GameFlowCoordinator replayed;
    CHECK(replayed.setDeterminismExchange(&playback));
    CHECK(replayed.setProgram(&fixture.program, &playbackRegistry));
    CHECK(replayed.request("start", {{"profile", "p2"}}));
    replayed.update();
    CHECK(replayed.request("finish", {{"score", std::int64_t(3)}}));
    replayed.update();
    CHECK(replayed.cancelSubflowCall("different ignored text"));
    CHECK(replayed.currentState() == "cancelled");
    CHECK(replayed.determinismHealthy());
    CHECK(fixture.guardCalls == 0);
    CHECK(fixture.pendingCalls == 0);
    CHECK(fixture.cancellationCalls == 0);
}

TEST_CASE(program_manifest_rejects_behavior_drift_before_runtime_mutation)
{
    DeterminismFixture fixture;
    const auto originalFingerprint = gameFlowProgramFingerprint(
        fixture.program);
    GameFlowProgram metadataOnly = fixture.program;
    metadataOnly.plans.at("root").document.extensions.emplace(
        "com.aliyat.editor", GameFlowValue::Object{{"x", 42.0}});
    CHECK(gameFlowProgramFingerprint(metadataOnly) == originalFingerprint);
    GameFlowProgram malformedNormalization = fixture.program;
    malformedNormalization.plans.at("root")
        .transitionsByStateAndIntent.clear();
    CHECK(gameFlowProgramFingerprint(malformedNormalization)
        != originalFingerprint);

    GameFlowDocument changedRoot = fixture.root;
    changedRoot.transitions[0].toState = "failed";
    GameFlowProgram changedProgram;
    CHECK(buildGameFlowProgram(changedRoot, fixture.registry,
        [&fixture](std::string_view id, GameFlowDocument& document,
                   std::string&) {
            if (id != fixture.child.id) return false;
            document = fixture.child;
            return true;
        }, changedProgram));
    CHECK(gameFlowProgramFingerprint(changedProgram) != originalFingerprint);

    MemoryExchange capture(GameFlowDeterminismMode::Capture);
    GameFlowCoordinator live;
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setProgram(&fixture.program, &fixture.registry));
    CHECK(capture.records.size() == 1u);
    CHECK(capture.records.front().kind
        == GameFlowDeterminismKind::ProgramManifest);

    MemoryExchange playback(GameFlowDeterminismMode::Playback);
    playback.records = capture.records;
    GameFlowCoordinator replayed;
    CHECK(replayed.setDeterminismExchange(&playback));
    std::string error;
    CHECK_FALSE(replayed.setProgram(
        &changedProgram, &fixture.registry, {}, &error));
    CHECK(error.find("programFingerprint") != std::string::npos);
    CHECK_FALSE(replayed.determinismHealthy());
    CHECK(replayed.currentState().empty());
    CHECK(replayed.queuedIntentCount() == 0u);
    CHECK(playback.position == 1u);
}

TEST_CASE(callback_requests_are_injected_in_order_without_live_side_effects)
{
    ReentrantFixture fixture;
    MemoryExchange capture(GameFlowDeterminismMode::Capture);
    GameFlowCoordinator live;
    fixture.coordinator = &live;
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setPlan(&fixture.plan, &fixture.registry));
    CHECK(live.request("go"));
    live.update();
    CHECK(live.pendingAction() != 0u);
    CHECK(live.queuedIntentCount() == 2u);
    CHECK(live.cancelActive("captured cancellation"));
    CHECK(live.currentState() == "cancelled");
    CHECK(live.queuedIntentCount() == 3u);
    live.update();
    CHECK(live.currentState() == "recovered");
    CHECK(live.queuedIntentCount() == 0u);
    CHECK(fixture.guardCalls == 1);
    CHECK(fixture.actionCalls == 1);
    CHECK(fixture.cancellationCalls == 1);
    CHECK(std::count_if(capture.records.begin(), capture.records.end(),
        [](const auto& record) {
            return record.kind == GameFlowDeterminismKind::Intent
                && record.intentOrigin
                    != GameFlowDeterminismIntentOrigin::External;
        }) == 3);
    CHECK(std::any_of(capture.records.begin(), capture.records.end(),
        [](const auto& record) {
            return record.kind == GameFlowDeterminismKind::Cancellation
                && record.cancellation
                    == GameFlowDeterminismCancellation::ActiveTransition;
        }));

    MemoryExchange playback(GameFlowDeterminismMode::Playback);
    playback.records = capture.records;
    GameFlowActionRegistry types;
    CHECK(types.registerGuardType({"test.reentrant-guard", {}}));
    CHECK(types.registerActionType(
        {"test.reentrant-action", {}, true}));
    GameFlowCoordinator replayed;
    CHECK(replayed.setDeterminismExchange(&playback));
    CHECK(replayed.setPlan(&fixture.plan, &types));
    CHECK(replayed.request("go"));
    replayed.update();
    CHECK(replayed.pendingAction() != 0u);
    CHECK(replayed.queuedIntentCount() == 2u);
    CHECK(replayed.cancelActive("ignored playback text"));
    CHECK(replayed.currentState() == "cancelled");
    CHECK(replayed.queuedIntentCount() == 3u);
    replayed.update();
    CHECK(replayed.currentState() == "recovered");
    CHECK(replayed.queuedIntentCount() == 0u);
    CHECK(replayed.determinismHealthy());
    CHECK(playback.position == playback.records.size());
    CHECK(fixture.guardCalls == 1);
    CHECK(fixture.actionCalls == 1);
    CHECK(fixture.cancellationCalls == 1);
}

TEST_CASE(timeout_is_replayed_and_does_not_run_live_cancellation)
{
    int actionCalls = 0;
    int cancellationCalls = 0;
    GameFlowActionRegistry registry;
    CHECK(registry.registerAction({"test.timeout", {}, true},
        [&](const GameFlowActionInvocation&) {
            ++actionCalls;
            return GameFlowActionResult::pending(
                [&]() { ++cancellationCalls; });
        }));
    GameFlowDocument document;
    document.id = "timeout";
    document.initialState = "idle";
    document.intents = {{"go"}};
    document.states = {{"idle"}, {"done"}, {"failed"}};
    GameFlowTransitionDefinition transition;
    transition.id = "timed";
    transition.fromState = "idle";
    transition.triggerIntent = "go";
    transition.toState = "done";
    transition.actions = {{"test.timeout", {}}};
    transition.onFailureState = "failed";
    transition.timeoutSeconds = 0.25;
    document.transitions.push_back(std::move(transition));
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));

    MemoryExchange capture(GameFlowDeterminismMode::Capture);
    GameFlowCoordinator live;
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setPlan(&plan, &registry));
    CHECK(live.request("go"));
    live.update();
    CHECK(live.pendingAction() != 0u);
    live.update(0.25);
    CHECK(live.currentState() == "failed");
    CHECK(actionCalls == 1);
    CHECK(cancellationCalls == 1);

    MemoryExchange playback(GameFlowDeterminismMode::Playback);
    playback.records = capture.records;
    GameFlowActionRegistry types;
    CHECK(types.registerActionType({"test.timeout", {}, true}));
    GameFlowCoordinator replayed;
    CHECK(replayed.setDeterminismExchange(&playback));
    CHECK(replayed.setPlan(&plan, &types));
    CHECK(replayed.request("go"));
    replayed.update();
    CHECK(replayed.pendingAction() != 0u);
    replayed.update(0.25);
    CHECK(replayed.currentState() == "failed");
    CHECK(replayed.determinismHealthy());
    CHECK(playback.position == playback.records.size());
    CHECK(actionCalls == 1);
    CHECK(cancellationCalls == 1);
}

TEST_CASE(observer_requests_are_consumed_once_or_injected_when_observer_absent)
{
    GameFlowActionRegistry registry;
    GameFlowDocument document;
    document.id = "observer";
    document.initialState = "idle";
    document.intents = {{"go"}, {"follow"}};
    document.states = {{"idle"}, {"middle"}, {"done"}};
    GameFlowTransitionDefinition go;
    go.id = "go";
    go.fromState = "idle";
    go.triggerIntent = "go";
    go.toState = "middle";
    GameFlowTransitionDefinition follow;
    follow.id = "follow";
    follow.fromState = "middle";
    follow.triggerIntent = "follow";
    follow.toState = "done";
    document.transitions = {go, follow};
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));

    MemoryExchange capture(GameFlowDeterminismMode::Capture);
    GameFlowCoordinator live;
    bool captureRequested = false;
    live.setEventObserver([&](const GameFlowEvent& event) {
        if (captureRequested
            || event.kind != GameFlowEventKind::IntentQueued) return;
        captureRequested = true;
        CHECK(live.request("follow"));
    });
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setPlan(&plan, &registry));
    CHECK(live.request("go"));
    live.update();
    CHECK(live.currentState() == "done");

    MemoryExchange matchingPlayback(GameFlowDeterminismMode::Playback);
    matchingPlayback.records = capture.records;
    GameFlowCoordinator matching;
    bool playbackRequested = false;
    matching.setEventObserver([&](const GameFlowEvent& event) {
        if (playbackRequested
            || event.kind != GameFlowEventKind::IntentQueued) return;
        playbackRequested = true;
        CHECK(matching.request("follow"));
    });
    CHECK(matching.setDeterminismExchange(&matchingPlayback));
    CHECK(matching.setPlan(&plan, &registry));
    CHECK(matching.request("go"));
    matching.update();
    CHECK(matching.currentState() == "done");
    CHECK(matching.determinismHealthy());
    CHECK(matchingPlayback.position == matchingPlayback.records.size());

    MemoryExchange observerlessPlayback(GameFlowDeterminismMode::Playback);
    observerlessPlayback.records = capture.records;
    GameFlowCoordinator observerless;
    CHECK(observerless.setDeterminismExchange(&observerlessPlayback));
    CHECK(observerless.setPlan(&plan, &registry));
    CHECK(observerless.request("go"));
    observerless.update();
    CHECK(observerless.currentState() == "done");
    CHECK(observerless.determinismHealthy());
    CHECK(observerlessPlayback.position
        == observerlessPlayback.records.size());
}

TEST_CASE(first_playback_mismatch_is_sticky_and_stops_mutation)
{
    DeterminismFixture fixture;
    MemoryExchange playback(GameFlowDeterminismMode::Playback);
    GameFlowDeterminismRecord wrong;
    wrong.schemaVersion = kGameFlowDeterminismSchemaVersion;
    wrong.sequence = 1u;
    wrong.kind = GameFlowDeterminismKind::Update;
    wrong.flowId = "root";
    wrong.stateId = "menu";
    playback.records.push_back(wrong);

    GameFlowCoordinator coordinator;
    CHECK(coordinator.setDeterminismExchange(&playback));
    CHECK_FALSE(coordinator.setProgram(&fixture.program, &fixture.registry));
    const auto result = coordinator.request(
        "start", {{"profile", "p3"}});
    CHECK(result.state == GameFlowRequestState::DeterminismFault);
    CHECK_FALSE(coordinator.determinismHealthy());
    const std::string firstFault(coordinator.determinismFault());
    CHECK_FALSE(firstFault.empty());
    CHECK(coordinator.queuedIntentCount() == 0u);
    CHECK(playback.position == 1u);

    coordinator.update(9.0);
    CHECK(coordinator.currentState().empty());
    CHECK(coordinator.queuedIntentCount() == 0u);
    CHECK(playback.position == 1u);
    CHECK(coordinator.determinismFault() == firstFault);

    MemoryExchange replacement(GameFlowDeterminismMode::Capture);
    CHECK_FALSE(coordinator.setDeterminismExchange(&replacement));
    CHECK(coordinator.determinismFault() == firstFault);
    CHECK(coordinator.setDeterminismExchange(nullptr));
    CHECK(coordinator.determinismHealthy());
}

TEST_CASE(observer_may_request_but_cannot_reenter_runtime_mutation)
{
    DeterminismFixture fixture;
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setProgram(&fixture.program, &fixture.registry));
    bool nested = false;
    bool cancelled = true;
    coordinator.setEventObserver([&](const GameFlowEvent& event) {
        if (nested || event.kind != GameFlowEventKind::IntentQueued) return;
        nested = true;
        coordinator.update();
        cancelled = coordinator.cancelActive();
        coordinator.reset();
        CHECK(coordinator.request("start", {{"profile", "nested"}}));
    });

    CHECK(coordinator.request("start", {{"profile", "outer"}}));
    CHECK(nested);
    CHECK_FALSE(cancelled);
    CHECK(coordinator.currentState() == "menu");
    CHECK(coordinator.queuedIntentCount() == 2u);
}

TEST_SUITE_END

#include <AYApplication/GameFlowProgram.h>
#include <AYTest.h>

#include <map>
#include <string>
#include <utility>

namespace
{

using namespace ayt::app;

GameFlowActionCall control(
    std::string id, GameFlowPayload arguments = {})
{
    return {std::move(id), std::move(arguments)};
}

GameFlowDocument makeChildFlow(bool addTrailingAction = false)
{
    GameFlowDocument child;
    child.id = "match";
    child.initialState = "waiting";
    child.entryParameters = {
        {"profileId", GameFlowValueType::String, true, {}},
    };
    child.result = {
        {"outcome", GameFlowValueType::String, true, "won"},
    };
    child.intents = {
        {"finish", {
            {"outcome", GameFlowValueType::String, false, "won"},
        }},
        {"work", {}},
    };
    child.states = {{"waiting"}, {"returned"}, {"failed"}};

    GameFlowTransitionDefinition finish;
    finish.id = "return_result";
    finish.fromState = "waiting";
    finish.triggerIntent = "finish";
    finish.toState = "returned";
    finish.actions = {
        {"test.observe-child", {}},
        control(std::string(kGameFlowActionReturn)),
    };
    if (addTrailingAction) finish.actions.push_back({"test.after-return", {}});
    finish.onFailureState = "failed";
    child.transitions.push_back(std::move(finish));

    GameFlowTransitionDefinition work;
    work.id = "pending_work";
    work.fromState = "waiting";
    work.triggerIntent = "work";
    work.toState = "returned";
    work.actions = {{"test.pending", {}}};
    work.onFailureState = "failed";
    child.transitions.push_back(std::move(work));
    return child;
}

GameFlowDocument makeRootFlow(double timeoutSeconds = 0.0)
{
    GameFlowDocument root;
    root.id = "root";
    root.initialState = "menu";
    root.intents = {
        {"begin", {
            {"profileId", GameFlowValueType::String, true, {}},
        }},
        {"after", {}},
    };
    root.states = {
        {"menu"}, {"done"}, {"complete"}, {"failed"}, {"cancelled"},
    };

    GameFlowTransitionDefinition launch;
    launch.id = "launch_match";
    launch.fromState = "menu";
    launch.triggerIntent = "begin";
    launch.toState = "done";
    launch.actions = {
        control(std::string(kGameFlowActionEnter), {
            {std::string(kGameFlowSubflowIdArgument), "match"},
        }),
        {"test.observe-parent", {}},
    };
    launch.onFailureState = "failed";
    launch.onCancelState = "cancelled";
    launch.timeoutSeconds = timeoutSeconds;
    root.transitions.push_back(std::move(launch));
    root.transitions.push_back(
        {"finish_parent", "done", "after", "complete"});
    return root;
}

struct SubflowHarness
{
    GameFlowActionRegistry registry;
    GameFlowDocument root = makeRootFlow();
    GameFlowDocument child = makeChildFlow();
    GameFlowProgram program;
    std::string childProfile;
    std::string parentOutcome;
    std::string returnedFlow;
    GameFlowActionExecutionId pending = 0;
    int cancellations = 0;

    SubflowHarness()
    {
        CHECK(registry.registerAction(
            {"test.observe-child", {}, false},
            [this](const GameFlowActionInvocation& invocation) {
                childProfile = std::get<std::string>(
                    invocation.flowParameters->at("profileId").data);
                return GameFlowActionResult::succeeded();
            }));
        CHECK(registry.registerAction(
            {"test.observe-parent", {}, false},
            [this](const GameFlowActionInvocation& invocation) {
                parentOutcome = std::get<std::string>(
                    invocation.lastSubflowResult->at("outcome").data);
                returnedFlow = invocation.returnedFlowId;
                return GameFlowActionResult::succeeded();
            }));
        CHECK(registry.registerAction(
            {"test.pending", {}, true},
            [this](const GameFlowActionInvocation& invocation) {
                pending = invocation.executionId;
                return GameFlowActionResult::pending([this]() {
                    ++cancellations;
                });
            }));
        CHECK(registry.registerAction(
            {"test.after-return", {}, false},
            [](const GameFlowActionInvocation&) {
                return GameFlowActionResult::succeeded();
            }));
    }

    bool build(std::vector<GameFlowDiagnostic>* diagnostics = nullptr,
               GameFlowProgramBuildOptions options = {})
    {
        return buildGameFlowProgram(root, registry,
            [this](std::string_view id,
                   GameFlowDocument& document,
                   std::string& error) {
                if (id != child.id) {
                    error = "Unknown subflow: " + std::string(id);
                    return false;
                }
                document = child;
                return true;
            }, program, diagnostics, options);
    }
};

} // namespace

TEST_SUITE(GameFlowSubflowTests)

TEST_CASE(subflow_parameters_and_results_resume_the_parent_transition)
{
    SubflowHarness harness;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(harness.build(&diagnostics));
    CHECK(diagnostics.empty());
    CHECK(harness.program.rootFlowId == "root");
    CHECK(harness.program.plans.size() == 2u);

    GameFlowCoordinator coordinator;
    CHECK(coordinator.setProgram(&harness.program, &harness.registry));
    CHECK(coordinator.currentFlow() == "root");
    CHECK(coordinator.qualifiedState() == "root::menu");
    CHECK(coordinator.request("begin", {{"profileId", "player-7"}}));
    CHECK(coordinator.request("after"));
    coordinator.update();

    const auto nested = coordinator.snapshot();
    CHECK(nested.status == GameFlowCoordinatorStatus::WaitingForSubflow);
    CHECK(nested.currentFlowId == "match");
    CHECK(nested.qualifiedStateId == "match::waiting");
    CHECK(nested.callDepth == 1u);
    CHECK(nested.frames.size() == 2u);
    CHECK(nested.frames[0].suspendedTransitionId == "launch_match");
    CHECK(nested.queuedIntentCount == 1u);
    const auto crashSnapshot = coordinator.diagnosticsSnapshot();
    CHECK(crashSnapshot.runtime.frames.size() == 2u);
    CHECK(crashSnapshot.runtime.frames[0].flowId == "root");
    CHECK(crashSnapshot.runtime.frames[0].suspendedTransitionId
        == "launch_match");
    CHECK(crashSnapshot.runtime.frames[1].flowId == "match");

    CHECK(coordinator.request("finish"));
    coordinator.update();
    CHECK(harness.childProfile == "player-7");
    CHECK(harness.parentOutcome == "won");
    CHECK(harness.returnedFlow == "match");
    CHECK(coordinator.currentFlow() == "root");
    CHECK(coordinator.currentState() == "complete");
    CHECK(coordinator.callDepth() == 0u);
    CHECK_FALSE(coordinator.busy());
    CHECK(coordinator.queuedIntentCount() == 0u);
    CHECK(coordinator.metrics().subflowsEntered == 1u);
    CHECK(coordinator.metrics().subflowsReturned == 1u);
    CHECK(coordinator.metrics().maxCallDepth == 1u);

    bool sawChildTrace = false;
    for (const auto& entry : coordinator.trace()) {
        if (entry.flowId == "match" && entry.callDepth == 1u) {
            sawChildTrace = true;
        }
    }
    CHECK(sawChildTrace);
}

TEST_CASE(program_compiler_rejects_unresolved_cycles_and_invalid_returns)
{
    {
        SubflowHarness harness;
        harness.root.transitions[0].actions[0].arguments["subflowId"] =
            "missing";
        std::vector<GameFlowDiagnostic> diagnostics;
        CHECK_FALSE(harness.build(&diagnostics));
        CHECK_FALSE(diagnostics.empty());
    }
    {
        SubflowHarness harness;
        harness.child.transitions[0].actions.insert(
            harness.child.transitions[0].actions.begin(),
            control(std::string(kGameFlowActionEnter), {
                {std::string(kGameFlowSubflowIdArgument), "root"},
            }));
        std::vector<GameFlowDiagnostic> diagnostics;
        CHECK_FALSE(buildGameFlowProgram(harness.root, harness.registry,
            [&harness](std::string_view id, GameFlowDocument& document,
                       std::string&) {
                document = id == "root" ? harness.root : harness.child;
                return id == "root" || id == "match";
            }, harness.program, &diagnostics));
        CHECK_FALSE(diagnostics.empty());
    }
    {
        SubflowHarness harness;
        harness.root.transitions[0].actions = {
            control(std::string(kGameFlowActionReturn)),
        };
        std::vector<GameFlowDiagnostic> diagnostics;
        CHECK_FALSE(harness.build(&diagnostics));
    }
    {
        SubflowHarness harness;
        harness.child = makeChildFlow(true);
        std::vector<GameFlowDiagnostic> diagnostics;
        CHECK_FALSE(harness.build(&diagnostics));
    }
}

TEST_CASE(control_ids_are_reserved_and_single_plan_execution_rejects_them)
{
    GameFlowActionRegistry registry;
    std::string error;
    CHECK_FALSE(registry.registerActionType(
        {std::string(kGameFlowActionEnter), {}, false}, false, &error));
    CHECK(error.find("reserved") != std::string::npos);
    CHECK_FALSE(registry.registerAction(
        {std::string(kGameFlowActionReturn), {}, false},
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::succeeded();
        }, false, &error));
    CHECK(error.find("reserved") != std::string::npos);
    CHECK(registry.registerAction(
        {"test.observe-parent", {}, false},
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::succeeded();
        }));

    GameFlowDocument root = makeRootFlow();
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(root, registry, plan));
    GameFlowCoordinator coordinator;
    CHECK_FALSE(coordinator.setPlan(&plan, &registry, &error));
    CHECK(error.find("GameFlowProgram") != std::string::npos);
}

TEST_CASE(program_compiler_checks_max_depth_across_shared_subflows)
{
    GameFlowActionRegistry registry;
    GameFlowDocument root;
    root.id = "root";
    root.initialState = "ready";
    root.intents = {{"to_a", {}}, {"to_b", {}}};
    root.states = {{"ready"}, {"done"}, {"failed"}};
    root.transitions = {
        {"first_resolve_a", "ready", "to_a", "done", {},
            {{std::string(kGameFlowActionEnter), {{
                std::string(kGameFlowSubflowIdArgument), "a"}}}},
            "failed"},
        {"then_resolve_b", "ready", "to_b", "done", {},
            {{std::string(kGameFlowActionEnter), {{
                std::string(kGameFlowSubflowIdArgument), "b"}}}},
            "failed"},
    };

    GameFlowDocument a;
    a.id = "a";
    a.initialState = "idle";
    a.states = {{"idle"}};

    GameFlowDocument b;
    b.id = "b";
    b.initialState = "idle";
    b.intents = {{"deeper", {}}};
    b.states = {{"idle"}, {"done"}, {"failed"}};
    b.transitions = {{"enter_a", "idle", "deeper", "done", {},
        {{std::string(kGameFlowActionEnter), {{
            std::string(kGameFlowSubflowIdArgument), "a"}}}}, "failed"}};

    GameFlowProgram program;
    std::vector<GameFlowDiagnostic> diagnostics;
    GameFlowProgramBuildOptions options;
    options.maxCallDepth = 2u;
    CHECK_FALSE(buildGameFlowProgram(root, registry,
        [&a, &b](std::string_view id, GameFlowDocument& document,
                 std::string&) {
            if (id == "a") document = a;
            else if (id == "b") document = b;
            else return false;
            return true;
        }, program, &diagnostics, options));
    CHECK_FALSE(diagnostics.empty());
}

TEST_CASE(parent_timeout_cancels_nested_pending_work_and_stales_completion)
{
    SubflowHarness harness;
    harness.root = makeRootFlow(1.0);
    CHECK(harness.build());
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setProgram(&harness.program, &harness.registry));
    CHECK(coordinator.request("begin", {{"profileId", "player-9"}}));
    coordinator.update();
    CHECK(coordinator.request("work"));
    coordinator.update();
    const auto stale = harness.pending;
    CHECK(stale != 0u);
    CHECK(coordinator.snapshot().status
        == GameFlowCoordinatorStatus::WaitingForAction);

    coordinator.update(1.0);
    CHECK(harness.cancellations == 1);
    CHECK(coordinator.callDepth() == 0u);
    CHECK(coordinator.currentFlow() == "root");
    CHECK(coordinator.currentState() == "failed");
    CHECK_FALSE(coordinator.busy());
    CHECK(coordinator.metrics().transitionsTimedOut == 1u);
    CHECK(coordinator.metrics().transitionsFailed == 1u);
    CHECK(coordinator.metrics().actionsCancelled == 1u);
    CHECK(coordinator.metrics().subflowsCancelled == 1u);
    std::string error;
    CHECK_FALSE(coordinator.completeAction(
        stale, GameFlowActionResult::succeeded(), &error));
    CHECK(error.find("stale") != std::string::npos);
}

TEST_CASE(cancel_subflow_call_unwinds_only_the_innermost_frame)
{
    SubflowHarness harness;
    harness.child.states.push_back({"cancelled"});
    harness.child.intents.push_back({"deeper", {}});
    GameFlowTransitionDefinition enterGrandchild;
    enterGrandchild.id = "enter_grandchild";
    enterGrandchild.fromState = "waiting";
    enterGrandchild.triggerIntent = "deeper";
    enterGrandchild.toState = "returned";
    enterGrandchild.actions = {control(std::string(kGameFlowActionEnter), {
        {std::string(kGameFlowSubflowIdArgument), "grandchild"},
    })};
    enterGrandchild.onFailureState = "failed";
    enterGrandchild.onCancelState = "cancelled";
    harness.child.transitions.push_back(std::move(enterGrandchild));

    GameFlowDocument grandchild;
    grandchild.id = "grandchild";
    grandchild.initialState = "idle";
    grandchild.states = {{"idle"}};

    CHECK(buildGameFlowProgram(harness.root, harness.registry,
        [&harness, &grandchild](std::string_view id,
                                GameFlowDocument& document,
                                std::string&) {
            if (id == harness.child.id) document = harness.child;
            else if (id == grandchild.id) document = grandchild;
            else return false;
            return true;
        }, harness.program));
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setProgram(&harness.program, &harness.registry));
    CHECK(coordinator.request("begin", {{"profileId", "player-10"}}));
    coordinator.update(0.0);
    CHECK(coordinator.currentFlow() == "match");
    CHECK(coordinator.request("deeper"));
    coordinator.update(0.0);
    CHECK(coordinator.currentFlow() == "grandchild");
    CHECK(coordinator.callDepth() == 2u);

    CHECK(coordinator.cancelSubflowCall("leave grandchild"));
    CHECK(coordinator.currentFlow() == "match");
    CHECK(coordinator.currentState() == "cancelled");
    CHECK(coordinator.callDepth() == 1u);
    CHECK(coordinator.snapshot().frames[0].suspendedTransitionId
        == "launch_match");

    CHECK(coordinator.cancelSubflowCall("leave child"));
    CHECK(coordinator.currentFlow() == "root");
    CHECK(coordinator.currentState() == "cancelled");
    CHECK(coordinator.callDepth() == 0u);
}

TEST_SUITE_END

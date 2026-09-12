#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/GameFlowCoordinator.h>
#include <AYApplication/GameFlowDocument.h>
#include <AYApplication/GameFlowMigration.h>
#include <AYTest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

using namespace ayt::app;

std::string readFixture(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / std::string(name);
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

GameFlowActionRegistry makeRegistry(
    std::vector<std::string>* records = nullptr,
    GameFlowActionExecutionId* pending = nullptr,
    bool* cancelled = nullptr)
{
    GameFlowActionRegistry registry;
    std::string error;
    CHECK(registry.registerAction(
        {"test.record",
            {{"label", GameFlowValueType::String, true, {}}},
            false},
        [records](const GameFlowActionInvocation& invocation) {
            if (records != nullptr) {
                const auto found = invocation.arguments->find("label");
                records->push_back(std::get<std::string>(found->second.data));
            }
            return GameFlowActionResult::succeeded();
        }, false, &error));
    CHECK(error.empty());
    CHECK(registry.registerAction(
        {"test.load", {}, true},
        [pending, cancelled](const GameFlowActionInvocation& invocation) {
            if (pending != nullptr) *pending = invocation.executionId;
            return GameFlowActionResult::pending([cancelled]() {
                if (cancelled != nullptr) *cancelled = true;
            });
        }, false, &error));
    CHECK(registry.registerGuard(
        {"test.allow", {}},
        [](const GameFlowGuardInvocation&) { return true; }, false, &error));
    CHECK(registry.registerGuard(
        {"test.deny", {}},
        [](const GameFlowGuardInvocation&) { return false; }, false, &error));
    return registry;
}

GameFlowDocument loadMainDocument()
{
    GameFlowDocument document;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(GameFlowSerializer::deserialize(
        readFixture("main.gameflow.json"), document, &diagnostics));
    CHECK(diagnostics.empty());
    return document;
}

GameFlowPlan buildMainPlan(const GameFlowActionRegistry& registry)
{
    GameFlowPlan plan;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(buildGameFlowPlan(loadMainDocument(), registry, plan, &diagnostics));
    CHECK(diagnostics.empty());
    return plan;
}

} // namespace

TEST_SUITE(GameFlowDocumentTests)

TEST_CASE(source_abi_tracks_the_public_action_metadata_layout)
{
    CHECK(kGameFlowSourceAbiVersion == 2u);
}

TEST_CASE(schema_v1_fixture_round_trips_without_losing_contract_data)
{
    GameFlowDocument document;
    GameFlowMigrationReport migration;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(GameFlowSerializer::deserialize(readFixture("main.gameflow.json"),
        document, &diagnostics, &migration));
    CHECK(migration.sourceVersion == 1u);
    CHECK(migration.targetVersion == kGameFlowSchemaVersion);
    CHECK(migration.changed);
    CHECK(migration.steps.size() == 1u);
    if (!migration.steps.empty()) {
        CHECK(migration.steps[0].fromVersion == 1u);
        CHECK(migration.steps[0].toVersion == 2u);
    }
    CHECK(document.schemaVersion == kGameFlowSchemaVersion);

    std::string encoded;
    CHECK(GameFlowSerializer::serialize(document, encoded, &diagnostics));
    CHECK(diagnostics.empty());

    GameFlowDocument decoded;
    CHECK(GameFlowSerializer::deserialize(encoded, decoded, &diagnostics));
    CHECK(decoded.id == document.id);
    CHECK(decoded.initialState == document.initialState);
    CHECK(decoded.intents.size() == document.intents.size());
    CHECK(decoded.states.size() == document.states.size());
    CHECK(decoded.transitions.size() == document.transitions.size());
    CHECK(decoded.transitions[1].timeoutSeconds == 5.0);
    CHECK(decoded.transitions[1].onFailureState == "load_error");
    CHECK(decoded.intents[1].payload[0].defaultValue
        == GameFlowValue(std::int64_t{0}));
}

TEST_CASE(json_values_reject_unsigned_integers_outside_int64_range)
{
    constexpr std::string_view document = R"json({
      "schemaVersion": 2,
      "id": "integer-range",
      "initialState": "idle",
      "entryParameters": [{
        "id": "slot",
        "type": "integer",
        "required": false,
        "default": 18446744073709551615
      }],
      "result": [],
      "extensions": {},
      "intents": [],
      "states": [{ "id": "idle" }],
      "transitions": []
    })json";
    GameFlowDocument decoded;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK_FALSE(GameFlowSerializer::deserialize(
        document, decoded, &diagnostics));
    CHECK_FALSE(diagnostics.empty());
}

TEST_CASE(transition_priority_requires_an_exact_signed_32_bit_integer)
{
    const auto documentWithPriority = [](std::string_view priority) {
        std::string document = R"json({
          "schemaVersion": 2,
          "id": "priority-range",
          "initialState": "idle",
          "entryParameters": [],
          "result": [],
          "extensions": {},
          "intents": [{ "id": "go" }],
          "states": [{ "id": "idle" }, { "id": "done" }],
          "transitions": [{
            "id": "go",
            "from": "idle",
            "intent": "go",
            "to": "done",
            "priority": __PRIORITY__
          }]
        })json";
        const auto marker = document.find("__PRIORITY__");
        document.replace(marker, std::string("__PRIORITY__").size(), priority);
        return document;
    };

    for (const std::string_view invalid : {
             "1.5", "2147483648", "-2147483649",
             "18446744073709551615"}) {
        GameFlowDocument decoded;
        std::vector<GameFlowDiagnostic> diagnostics;
        CHECK_FALSE(GameFlowSerializer::deserialize(
            documentWithPriority(invalid), decoded, &diagnostics));
        CHECK_FALSE(diagnostics.empty());
    }

    for (const std::string_view valid : {"2147483647", "-2147483648"}) {
        GameFlowDocument decoded;
        std::vector<GameFlowDiagnostic> diagnostics;
        CHECK(GameFlowSerializer::deserialize(
            documentWithPriority(valid), decoded, &diagnostics));
        CHECK(diagnostics.empty());
    }
}

TEST_CASE(raw_schema_migration_is_idempotent_and_reports_each_step)
{
    GameFlowMigrationReport firstReport;
    std::vector<GameFlowDiagnostic> diagnostics;
    std::string migrated;
    CHECK(migrateGameFlowJson(readFixture("main.gameflow.json"), migrated,
        &firstReport, &diagnostics, false));
    CHECK(diagnostics.empty());
    CHECK(firstReport.sourceVersion == 1u);
    CHECK(firstReport.targetVersion == 2u);
    CHECK(firstReport.changed);
    CHECK(firstReport.steps.size() == 1u);
    CHECK(migrated.find("\"schemaVersion\":2") != std::string::npos);
    CHECK(migrated.find("\"entryParameters\":[]") != std::string::npos);
    CHECK(migrated.find("\"result\":[]") != std::string::npos);
    CHECK(migrated.find("\"extensions\":{}") != std::string::npos);

    GameFlowMigrationReport secondReport;
    std::string migratedAgain;
    CHECK(migrateGameFlowJson(migrated, migratedAgain, &secondReport,
        &diagnostics, false));
    CHECK(migratedAgain == migrated);
    CHECK(secondReport.sourceVersion == 2u);
    CHECK(secondReport.targetVersion == 2u);
    CHECK_FALSE(secondReport.changed);
    CHECK(secondReport.steps.empty());
}

TEST_CASE(schema_v2_preserves_subflow_contracts_and_namespaced_extensions)
{
    GameFlowDocument document;
    GameFlowMigrationReport migration;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(GameFlowSerializer::deserialize(
        readFixture("schema-v2.gameflow.json"), document, &diagnostics,
        &migration));
    CHECK_FALSE(migration.changed);
    CHECK(document.schemaVersion == 2u);
    CHECK(document.entryParameters.size() == 1u);
    if (!document.entryParameters.empty()) {
        CHECK(document.entryParameters[0].id == "profileId");
        CHECK(document.entryParameters[0].required);
    }
    CHECK(document.result.size() == 1u);
    if (!document.result.empty()) {
        CHECK(document.result[0].id == "outcome");
    }
    CHECK(document.extensions.contains("com.aliyat.editor"));

    std::string encoded;
    CHECK(GameFlowSerializer::serialize(document, encoded, &diagnostics));
    GameFlowDocument decoded;
    CHECK(GameFlowSerializer::deserialize(encoded, decoded, &diagnostics));
    CHECK(decoded.entryParameters.size() == 1u);
    if (!decoded.entryParameters.empty() && !document.entryParameters.empty()) {
        CHECK(decoded.entryParameters[0].id == document.entryParameters[0].id);
        CHECK(decoded.entryParameters[0].type
            == document.entryParameters[0].type);
        CHECK(decoded.entryParameters[0].required
            == document.entryParameters[0].required);
    }
    CHECK(decoded.result.size() == 1u);
    if (!decoded.result.empty() && !document.result.empty()) {
        CHECK(decoded.result[0].id == document.result[0].id);
        CHECK(decoded.result[0].type == document.result[0].type);
        CHECK(decoded.result[0].required == document.result[0].required);
    }
    CHECK(decoded.extensions == document.extensions);
}

TEST_CASE(future_schema_is_rejected_without_modifying_destination)
{
    std::string migrated = "leave-this-value";
    GameFlowMigrationReport migration;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK_FALSE(migrateGameFlowJson(
        readFixture("future.gameflow.json"), migrated, &migration,
        &diagnostics));
    CHECK(migrated == "leave-this-value");
    CHECK(migration.sourceVersion == 3u);
    CHECK_FALSE(migration.changed);
    CHECK(diagnostics.size() == 1u);
    if (!diagnostics.empty()) {
        CHECK(diagnostics[0].path == "$.schemaVersion");
        CHECK(diagnostics[0].message.find("newer") != std::string::npos);
    }

    GameFlowDocument destination;
    destination.id = "unchanged";
    CHECK_FALSE(GameFlowSerializer::deserialize(
        readFixture("future.gameflow.json"), destination, &diagnostics));
    CHECK(destination.id == "unchanged");
}

TEST_CASE(invalid_fixture_reports_reference_duplicates_and_hierarchy_errors)
{
    GameFlowDocument document;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(!GameFlowSerializer::deserialize(
        readFixture("invalid.gameflow.json"), document, &diagnostics));
    CHECK(diagnostics.size() >= 4u);
}

TEST_CASE(normalization_requires_registered_actions_and_valid_arguments)
{
    auto document = loadMainDocument();
    GameFlowActionRegistry emptyRegistry;
    GameFlowPlan plan;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(!buildGameFlowPlan(document, emptyRegistry, plan, &diagnostics));

    auto registry = makeRegistry();
    document.transitions[0].actions[0].arguments["label"] = true;
    CHECK(!buildGameFlowPlan(document, registry, plan, &diagnostics));
}

TEST_CASE(authoring_can_register_and_enumerate_types_without_runtime_handlers)
{
    GameFlowActionRegistry registry;
    CHECK(registry.registerActionType(
        {"world.replace",
            {{"worldId", GameFlowValueType::String, true, {}}}, true}));
    CHECK(registry.registerGuardType({"save.exists", {}}));
    CHECK(registry.findActionHandler("world.replace") == nullptr);
    CHECK(registry.findGuardHandler("save.exists") == nullptr);
    CHECK(registry.actionTypes().size() == 1u);
    CHECK(registry.actionTypes()[0].id == "world.replace");
    CHECK(registry.guardTypes().size() == 1u);
}

TEST_CASE(action_reference_metadata_rejects_unknown_reference_kinds)
{
    GameFlowActionRegistry registry;
    GameFlowActionTypeDefinition definition{
        "asset.open",
        {{"path", GameFlowValueType::String, true, {}}},
        false,
        {{"path", static_cast<GameFlowReferenceKind>(0xffu), false}},
    };
    std::string error;
    CHECK_FALSE(registry.registerActionType(
        std::move(definition), false, &error));
    CHECK(error.find("invalid kind") != std::string::npos);
    CHECK(registry.findAction("asset.open") == nullptr);
}

TEST_CASE(normalized_transition_order_is_priority_then_document_order)
{
    auto registry = makeRegistry();
    auto document = loadMainDocument();
    auto preferred = document.transitions.front();
    preferred.id = "preferred";
    preferred.priority = 10;
    preferred.guard.guard = "test.allow";
    document.transitions.push_back(preferred);

    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    const auto& candidates = plan.transitionsByStateAndIntent.at(
        std::string("main_menu\x1fstart_game"));
    CHECK(candidates.size() == 2u);
    CHECK(plan.document.transitions[
        plan.transitions[candidates[0]].documentIndex].id == "preferred");
}

TEST_CASE(normalized_plan_materializes_registered_argument_defaults)
{
    std::string observed;
    GameFlowActionRegistry registry;
    CHECK(registry.registerAction(
        {"test.default",
            {{"mode", GameFlowValueType::String, true, "automatic"}}},
        [&observed](const GameFlowActionInvocation& invocation) {
            observed = std::get<std::string>(
                invocation.arguments->at("mode").data);
            return GameFlowActionResult::succeeded();
        }));
    GameFlowDocument document;
    document.id = "defaults";
    document.initialState = "before";
    document.intents = {{"go", {}}};
    document.states = {{"before"}, {"after"}};
    GameFlowTransitionDefinition transition;
    transition.id = "go_after";
    transition.fromState = "before";
    transition.triggerIntent = "go";
    transition.toState = "after";
    transition.actions.push_back({"test.default", {}});
    document.transitions.push_back(std::move(transition));

    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    CHECK(plan.document.transitions[0].actions[0].arguments.contains("mode"));
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("go"));
    coordinator.update();
    CHECK(observed == "automatic");
    CHECK(coordinator.currentState() == "after");
}

TEST_SUITE_END

TEST_SUITE(GameFlowCoordinatorTests)

TEST_CASE(runtime_snapshot_distinguishes_lifecycle_and_locates_pending_action)
{
    GameFlowActionExecutionId pending = 0;
    auto registry = makeRegistry(nullptr, &pending);
    auto plan = buildMainPlan(registry);
    GameFlowCoordinator coordinator;

    auto snapshot = coordinator.snapshot();
    CHECK(snapshot.status == GameFlowCoordinatorStatus::NotReady);
    CHECK(snapshot.currentStateId.empty());
    CHECK(snapshot.activeActionIndex == kNoGameFlowActionIndex);
    CHECK_FALSE(snapshot.busy);

    CHECK(coordinator.setPlan(&plan, &registry));
    snapshot = coordinator.snapshot();
    CHECK(snapshot.status == GameFlowCoordinatorStatus::Idle);
    CHECK(snapshot.currentStateId == "main_menu");

    CHECK(coordinator.request("start_game"));
    snapshot = coordinator.snapshot();
    CHECK(snapshot.status == GameFlowCoordinatorStatus::Queued);
    CHECK(snapshot.queuedIntentCount == 1u);
    CHECK_FALSE(snapshot.busy);
    coordinator.update();

    CHECK(coordinator.request("world_ready"));
    CHECK(coordinator.request("return_to_menu"));
    coordinator.update();
    snapshot = coordinator.snapshot();
    CHECK(snapshot.status == GameFlowCoordinatorStatus::WaitingForAction);
    CHECK(snapshot.currentStateId == "loading");
    CHECK(snapshot.activeTransitionId == "activate_world");
    CHECK(snapshot.activeActionId == "test.load");
    CHECK(snapshot.activeActionIndex == 0u);
    CHECK(snapshot.generation != 0u);
    CHECK(snapshot.executionId == pending);
    CHECK(snapshot.queuedIntentCount == 1u);
    CHECK(snapshot.busy);

    CHECK(coordinator.completeAction(
        pending, GameFlowActionResult::succeeded()));
    snapshot = coordinator.snapshot();
    CHECK(snapshot.status == GameFlowCoordinatorStatus::Queued);
    CHECK(snapshot.currentStateId == "playing");
    CHECK(snapshot.activeTransitionId.empty());
    CHECK(snapshot.activeActionId.empty());
    CHECK(snapshot.activeActionIndex == kNoGameFlowActionIndex);
    CHECK(snapshot.generation == 0u);
    CHECK(snapshot.executionId == 0u);
    CHECK(snapshot.queuedIntentCount == 1u);
    CHECK_FALSE(snapshot.busy);

    coordinator.update();
    snapshot = coordinator.snapshot();
    CHECK(snapshot.status == GameFlowCoordinatorStatus::Idle);
    CHECK(snapshot.currentStateId == "main_menu");
    CHECK(snapshot.queuedIntentCount == 0u);
}

TEST_CASE(runtime_snapshot_identifies_a_synchronous_action_while_it_executes)
{
    GameFlowCoordinator coordinator;
    GameFlowCoordinatorSnapshot observed;
    GameFlowActionRegistry registry;
    CHECK(registry.registerAction(
        {"test.inspect", {}, false},
        [&](const GameFlowActionInvocation& invocation) {
            observed = coordinator.snapshot();
            CHECK(observed.generation == invocation.generation);
            CHECK(observed.executionId == invocation.executionId);
            return GameFlowActionResult::succeeded();
        }));

    GameFlowDocument document;
    document.id = "snapshot";
    document.initialState = "before";
    document.intents = {{"go", {}}};
    document.states = {{"before"}, {"after"}};
    GameFlowTransitionDefinition transition;
    transition.id = "inspect";
    transition.fromState = "before";
    transition.triggerIntent = "go";
    transition.toState = "after";
    transition.actions.push_back({"test.inspect", {}});
    document.transitions.push_back(std::move(transition));

    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("go"));
    coordinator.update();

    CHECK(observed.status == GameFlowCoordinatorStatus::Running);
    CHECK(observed.currentStateId == "before");
    CHECK(observed.activeTransitionId == "inspect");
    CHECK(observed.activeActionId == "test.inspect");
    CHECK(observed.activeActionIndex == 0u);
    CHECK(observed.busy);
    CHECK(coordinator.currentState() == "after");
    CHECK(coordinator.snapshot().status == GameFlowCoordinatorStatus::Idle);
}

TEST_CASE(denied_high_priority_transition_falls_back_deterministically)
{
    std::vector<std::string> records;
    auto registry = makeRegistry(&records);
    auto document = loadMainDocument();
    auto denied = document.transitions.front();
    denied.id = "denied_preferred";
    denied.priority = 100;
    denied.guard.guard = "test.deny";
    document.transitions.push_back(std::move(denied));
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));

    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("start_game"));
    coordinator.update();
    CHECK(coordinator.currentState() == "loading");
    CHECK(records.size() == 1u);
}

TEST_CASE(headless_flow_moves_from_menu_through_loading_to_playing)
{
    std::vector<std::string> records;
    GameFlowActionExecutionId pending = 0;
    auto registry = makeRegistry(&records, &pending);
    auto plan = buildMainPlan(registry);
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.currentState() == "main_menu");

    CHECK(coordinator.request("start_game"));
    coordinator.update();
    CHECK(coordinator.currentState() == "loading");
    CHECK(records.size() == 1u);
    CHECK(records[0] == "begin");

    CHECK(coordinator.request("world_ready"));
    coordinator.update();
    CHECK(coordinator.currentState() == "loading");
    CHECK(coordinator.busy());
    CHECK(pending != 0u);
    CHECK(coordinator.completeAction(
        pending, GameFlowActionResult::succeeded()));
    CHECK(coordinator.currentState() == "playing");
    CHECK(!coordinator.busy());
}

TEST_CASE(intent_payload_is_typed_and_receives_defaults)
{
    std::int64_t observedSlot = -1;
    GameFlowActionRegistry registry;
    CHECK(registry.registerAction(
        {"test.record", {{"label", GameFlowValueType::String, true, {}}}},
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::succeeded();
        }));
    CHECK(registry.registerAction(
        {"test.load", {}, true},
        [&observedSlot](const GameFlowActionInvocation& invocation) {
            observedSlot = std::get<std::int64_t>(
                invocation.intentPayload->at("slot").data);
            return GameFlowActionResult::succeeded();
        }));
    auto plan = buildMainPlan(registry);
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("start_game"));
    coordinator.update();
    CHECK(coordinator.request("world_ready"));
    coordinator.update();
    CHECK(observedSlot == 0);
    CHECK(coordinator.currentState() == "playing");

    const auto rejected = coordinator.request(
        "world_ready", {{"slot", GameFlowValue("wrong")}});
    CHECK(rejected.state == GameFlowRequestState::InvalidPayload);
}

TEST_CASE(failure_cancel_timeout_and_stale_completion_take_declared_routes)
{
    GameFlowActionExecutionId pending = 0;
    bool cancelled = false;
    auto registry = makeRegistry(nullptr, &pending, &cancelled);
    auto plan = buildMainPlan(registry);
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.request("start_game"));
    coordinator.update();
    CHECK(coordinator.request("world_ready"));
    coordinator.update();
    const auto stale = pending;
    CHECK(coordinator.cancelActive("user left"));
    CHECK(cancelled);
    CHECK(coordinator.currentState() == "main_menu");

    cancelled = false;
    CHECK(coordinator.request("start_game"));
    coordinator.update();
    CHECK(coordinator.request("world_ready"));
    coordinator.update();
    const auto current = pending;
    CHECK(current != stale);
    std::string error;
    CHECK(!coordinator.completeAction(
        stale, GameFlowActionResult::succeeded(), &error));
    CHECK(error.find("stale") != std::string::npos);
    CHECK(coordinator.currentState() == "loading");
    CHECK(coordinator.pendingAction() == current);
    CHECK(coordinator.completeAction(
        current, GameFlowActionResult::failed("load rejected")));
    CHECK(coordinator.currentState() == "load_error");

    CHECK(coordinator.setPlan(&plan, &registry));
    cancelled = false;
    CHECK(coordinator.request("start_game"));
    coordinator.update();
    CHECK(coordinator.request("world_ready"));
    coordinator.update();
    coordinator.update(5.0);
    CHECK(cancelled);
    CHECK(coordinator.currentState() == "load_error");
}

TEST_CASE(child_state_can_use_parent_transition)
{
    auto registry = makeRegistry();
    GameFlowDocument document;
    document.id = "hierarchy";
    document.initialState = "game";
    document.intents = {{"pause", {}}};
    document.states = {
        {"game", {}, "playing"},
        {"playing", "game", {}},
        {"paused", {}, {}},
    };
    document.transitions = {
        {"pause_game", "game", "pause", "paused"},
    };
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    GameFlowCoordinator coordinator;
    CHECK(coordinator.setPlan(&plan, &registry));
    CHECK(coordinator.currentState() == "playing");
    CHECK(coordinator.request("pause"));
    coordinator.update();
    CHECK(coordinator.currentState() == "paused");
}

TEST_SUITE_END

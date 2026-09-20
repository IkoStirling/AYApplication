#include <AYApplication/GameFlowRuntime.h>
#include <AYApplication/GameFlowWorldActions.h>
#include <AYApplication/IEngineHost.h>
#include <AYEventSystem/EventBus.h>
#include <AYGameLoop.h>
#include <AYTest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{

using namespace ayt::app;

class RuntimeTestHost final : public IEngineHost
{
public:
    ayt::game::IGameLoop& gameLoop() override
    {
        return ayt::game::GameLoop::instance();
    }

    ayt::event::EventBus& eventBus() override
    {
        return _events;
    }

    ayt::game::ISubSystem* findSubSystem(const char*) override
    {
        return nullptr;
    }

    void provideService(std::string_view key, void* instance) override
    {
        if (key.empty()) return;
        if (instance == nullptr) {
            _services.erase(std::string(key));
        } else {
            _services[std::string(key)] = instance;
        }
    }

    void* findService(std::string_view key) const override
    {
        const auto found = _services.find(std::string(key));
        return found == _services.end() ? nullptr : found->second;
    }

    void clearProvidedServices() override
    {
        _services.clear();
    }

    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override { return nullptr; }
    ayt::physics::IPhysicsQuery* physicsQuery() override { return nullptr; }
    ayt::audio::AudioEngine* audio() override { return nullptr; }
    ayt::scene::SceneManager* scenes() override { return nullptr; }

private:
    ayt::event::EventBus _events;
    std::unordered_map<std::string, void*> _services;
};

std::filesystem::path startupFixture()
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / "startup.gameflow.json";
}

std::filesystem::path subflowFixture()
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / "runtime-subflow.gameflow.json";
}

std::filesystem::path schemaV2Fixture()
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / "schema-v2.gameflow.json";
}

std::filesystem::path reloadFixture()
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / "reload.gameflow.json";
}

std::filesystem::path pendingFixture()
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / "reload-pending.gameflow.json";
}

std::filesystem::path reentrantFixture()
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / "reload-reentrant.gameflow.json";
}

GameFlowDocument startupChildFlow()
{
    GameFlowDocument child;
    child.id = "runtime-child";
    child.initialState = "waiting";
    child.intents = {{"finish", {}}};
    child.states = {{"waiting"}, {"returned"}};
    GameFlowTransitionDefinition transition;
    transition.id = "return_to_root";
    transition.fromState = "waiting";
    transition.triggerIntent = "finish";
    transition.toState = "returned";
    transition.actions = {{std::string(kGameFlowActionReturn), {}}};
    child.transitions.push_back(std::move(transition));
    return child;
}

GameFlowRuntimeConfig runtimeConfig(bool& started)
{
    GameFlowRuntimeConfig config;
    config.documentPath = startupFixture().string();
    config.enableWorldActions = false;
    config.configureRegistry = [&started](
        GameFlowActionRegistry& registry,
        std::string& error) {
        return registry.registerAction(
            {"test.mark_started", {}, false},
            [&started](const GameFlowActionInvocation&) {
                started = true;
                return GameFlowActionResult::succeeded();
            },
            false,
            &error);
    };
    return config;
}

} // namespace

TEST_SUITE(GameFlowRuntimeTests)

TEST_CASE(preflight_builds_the_startup_plan_before_host_initialization)
{
    bool started = false;
    std::string error;
    auto prepared = prepareGameFlowRuntime(runtimeConfig(started), &error);

    CHECK_NOT_NULL(prepared.get());
    CHECK(error.empty());
    CHECK_FALSE(started);
    if (prepared != nullptr) {
        CHECK(prepared->documentPath() == startupFixture().string());
        CHECK(prepared->startupIntent() == "app.start");
        CHECK_FALSE(prepared->worldActionsEnabled());
        CHECK(prepared->document().id == "runtime-startup");
        CHECK(prepared->diagnostics().empty());
    }
}

TEST_CASE(startup_intent_runs_on_the_first_ingress_update)
{
    bool started = false;
    std::string error;
    auto prepared = prepareGameFlowRuntime(runtimeConfig(started), &error);
    CHECK_NOT_NULL(prepared.get());
    if (prepared == nullptr) return;

    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    CHECK(runtime.initialize());
    CHECK(runtime.ready());
    CHECK(runtime.currentState() == "boot");
    CHECK_FALSE(started);

    runtime.update(0.0f);
    CHECK(started);
    CHECK(runtime.currentState() == "ready");
    CHECK_FALSE(runtime.snapshot().busy);

    runtime.shutdown();
    CHECK_FALSE(runtime.ready());
    CHECK(runtime.currentState().empty());
}

TEST_CASE(preflight_rejects_an_undeclared_startup_intent)
{
    bool started = false;
    auto config = runtimeConfig(started);
    config.startupIntent = "application.missing";
    std::string error;
    auto prepared = prepareGameFlowRuntime(std::move(config), &error);

    CHECK(prepared == nullptr);
    CHECK(error.find("application.missing") != std::string::npos);
    CHECK(error.find("not declared") != std::string::npos);
}

TEST_CASE(preflight_reports_a_missing_document_before_runtime_creation)
{
    GameFlowRuntimeConfig config;
    config.documentPath =
        (std::filesystem::temp_directory_path()
         / "__ay_missing_startup.gameflow.json").string();
    config.enableWorldActions = false;
    std::string error;
    auto prepared = prepareGameFlowRuntime(std::move(config), &error);

    CHECK(prepared == nullptr);
    CHECK(error.find("Cannot open GameFlow document") != std::string::npos);
}

TEST_CASE(preflight_resolves_and_runtime_executes_a_subflow_program)
{
    GameFlowRuntimeConfig config;
    config.documentPath = subflowFixture().string();
    config.enableWorldActions = false;
    config.resolveDocument = [](std::string_view flowId,
                                 GameFlowDocument& document,
                                 std::string& error) {
        if (flowId != "runtime-child") {
            error = "Unknown test subflow: " + std::string(flowId);
            return false;
        }
        document = startupChildFlow();
        return true;
    };

    std::string error;
    auto prepared = prepareGameFlowRuntime(std::move(config), &error);
    CHECK_NOT_NULL(prepared.get());
    CHECK(error.empty());
    if (prepared == nullptr) return;

    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    CHECK(runtime.initialize());
    runtime.update(0.0f);
    CHECK(runtime.snapshot().currentFlowId == "runtime-child");
    CHECK(runtime.currentState() == "waiting");
    CHECK(runtime.snapshot().callDepth == 1u);

    CHECK(runtime.request("finish"));
    runtime.update(0.0f);
    CHECK(runtime.snapshot().currentFlowId == "runtime-root");
    CHECK(runtime.currentState() == "ready");
    CHECK(runtime.snapshot().callDepth == 0u);
}

TEST_CASE(preflight_contains_subflow_resolver_exceptions)
{
    GameFlowRuntimeConfig config;
    config.documentPath = subflowFixture().string();
    config.enableWorldActions = false;
    config.resolveDocument = [](std::string_view, GameFlowDocument&,
                                 std::string&) -> bool {
        throw std::runtime_error("resolver unavailable");
    };

    std::string error;
    auto prepared = prepareGameFlowRuntime(std::move(config), &error);
    CHECK(prepared == nullptr);
    CHECK(error.find("resolver threw") != std::string::npos);
    CHECK(error.find("resolver unavailable") != std::string::npos);
}

TEST_CASE(preflight_rejects_invalid_root_parameters)
{
    GameFlowRuntimeConfig config;
    config.documentPath = schemaV2Fixture().string();
    config.enableWorldActions = false;
    config.startupIntent.clear();

    std::string error;
    auto prepared = prepareGameFlowRuntime(config, &error);
    CHECK(prepared == nullptr);
    CHECK(error.find("profileId") != std::string::npos);

    config.rootParameters = {{"profileId", "player-1"}};
    prepared = prepareGameFlowRuntime(std::move(config), &error);
    CHECK_NOT_NULL(prepared.get());
    CHECK(error.empty());
}

TEST_CASE(reload_is_transactional_preserves_state_and_restores_bound_handlers)
{
    bool initiallyStarted = false;
    auto prepared = prepareGameFlowRuntime(runtimeConfig(initiallyStarted));
    CHECK_NOT_NULL(prepared.get());
    if (prepared == nullptr) return;

    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    CHECK(runtime.initialize());
    runtime.update(0.0f);
    CHECK(runtime.currentState() == "ready");

    bool boundHandlerRan = false;
    CHECK(runtime.bindActionHandler("test.mark_started",
        [&boundHandlerRan](const GameFlowActionInvocation&) {
            boundHandlerRan = true;
            return GameFlowActionResult::succeeded();
        }));

    GameFlowRuntimeConfig invalid;
    invalid.documentPath = "__missing_reload__.gameflow.json";
    invalid.enableWorldActions = false;
    const GameFlowReloadResult rejected = runtime.reload(std::move(invalid));
    CHECK(rejected.state == GameFlowReloadState::Rejected);
    CHECK(runtime.ready());
    CHECK(runtime.currentState() == "ready");
    CHECK(runtime.documentPath() == startupFixture().string());

    bool candidateHandlerRan = false;
    GameFlowRuntimeConfig drifted;
    drifted.documentPath = reloadFixture().string();
    drifted.enableWorldActions = false;
    drifted.configureRegistry = [](GameFlowActionRegistry& registry,
                                   std::string& error) {
        return registry.registerAction(
            {"test.mark_started", {}, true},
            [](const GameFlowActionInvocation&) {
                return GameFlowActionResult::succeeded();
            }, false, &error);
    };
    const GameFlowReloadResult driftRejected = runtime.reload(
        std::move(drifted));
    CHECK(driftRejected.state == GameFlowReloadState::Rejected);
    CHECK(driftRejected.message.find("contract") != std::string::npos);
    CHECK(runtime.documentPath() == startupFixture().string());

    auto candidate = runtimeConfig(candidateHandlerRan);
    candidate.documentPath = reloadFixture().string();
    const GameFlowReloadResult applied = runtime.reload(std::move(candidate));
    CHECK(applied.state == GameFlowReloadState::Applied);
    CHECK_FALSE(runtime.reloadPending());
    CHECK(runtime.lastReloadError().empty());
    CHECK(runtime.currentState() == "ready");
    CHECK(runtime.documentPath() == reloadFixture().string());

    CHECK(runtime.request("reload.only"));
    runtime.update(0.0f);
    CHECK(boundHandlerRan);
    CHECK_FALSE(candidateHandlerRan);
    CHECK(runtime.currentState() == "reloaded");
}

TEST_CASE(program_reloaded_observer_sees_the_new_active_document)
{
    bool initiallyStarted = false;
    auto prepared = prepareGameFlowRuntime(runtimeConfig(initiallyStarted));
    CHECK_NOT_NULL(prepared.get());
    if (prepared == nullptr) return;

    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    CHECK(runtime.initialize());
    runtime.update(0.0f);
    CHECK(runtime.bindActionHandler("test.mark_started",
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::succeeded();
        }));

    bool observed = false;
    std::string observedFlowId;
    bool observedReloadIntent = false;
    bool observedReloadProgram = false;
    bool observedRootDocument = false;
    bool observedCandidateRegistry = false;
    bool observedCandidatePath = false;
    bool observerUnboundOldAction = false;
    bool observerBoundCandidateAction = false;
    GameFlowReloadResult nestedReload;
    runtime.setEventObserver(
        [&](const GameFlowEvent& event) {
            if (event.kind != GameFlowEventKind::ProgramReloaded) return;
            observed = true;
            const GameFlowProgram* program = runtime.program();
            observedReloadProgram = program != nullptr
                && program->rootFlowId == "runtime-startup";
            const GameFlowDocument* root = runtime.document();
            observedRootDocument = root != nullptr
                && root->findIntent("reload.only") != nullptr;
            const GameFlowActionRegistry* registry = runtime.registry();
            observedCandidateRegistry = registry != nullptr
                && registry->findAction("reload.marker") != nullptr;
            observedCandidatePath =
                runtime.documentPath() == reloadFixture().string();
            const GameFlowDocument* active = runtime.activeDocument();
            if (active == nullptr) return;
            observedFlowId = active->id;
            observedReloadIntent =
                active->findIntent("reload.only") != nullptr;

            observerUnboundOldAction =
                runtime.unbindActionHandler("test.mark_started");
            observerBoundCandidateAction = runtime.bindActionHandler(
                "reload.marker",
                [](const GameFlowActionInvocation&) {
                    return GameFlowActionResult::succeeded();
                });

            bool nestedStarted = false;
            auto nested = runtimeConfig(nestedStarted);
            nested.documentPath = startupFixture().string();
            nestedReload = runtime.reload(std::move(nested));
        });

    bool candidateStarted = false;
    auto candidate = runtimeConfig(candidateStarted);
    candidate.documentPath = reloadFixture().string();
    const auto configureCandidate = std::move(candidate.configureRegistry);
    candidate.configureRegistry =
        [configureCandidate](GameFlowActionRegistry& registry,
                             std::string& error) {
            if (!configureCandidate(registry, error)) return false;
            return registry.registerActionType(
                {"reload.marker", {}, false}, false, &error);
        };
    const GameFlowReloadResult result = runtime.reload(std::move(candidate));

    CHECK(result.state == GameFlowReloadState::Applied);
    CHECK(observed);
    CHECK(observedReloadProgram);
    CHECK(observedRootDocument);
    CHECK(observedCandidateRegistry);
    CHECK(observedCandidatePath);
    CHECK(observedFlowId == "runtime-startup");
    CHECK(observedReloadIntent);
    CHECK(observerUnboundOldAction);
    CHECK(observerBoundCandidateAction);
    CHECK(nestedReload.state == GameFlowReloadState::Rejected);
    CHECK(nestedReload.message.find("already being applied")
        != std::string::npos);
    const GameFlowDocument* reloadedDocument = runtime.document();
    CHECK_NOT_NULL(reloadedDocument);
    if (reloadedDocument != nullptr) {
        CHECK(reloadedDocument->findIntent("reload.only") != nullptr);
    }
    CHECK(runtime.registry()->findActionHandler("test.mark_started")
        == nullptr);
    CHECK(runtime.registry()->findActionHandler("reload.marker") != nullptr);
}

TEST_CASE(reload_waits_for_pending_transition_then_applies_at_safe_point)
{
    GameFlowActionExecutionId pending = 0;
    GameFlowRuntimeConfig initial;
    initial.documentPath = pendingFixture().string();
    initial.enableWorldActions = false;
    initial.configureRegistry = [&pending](GameFlowActionRegistry& registry,
                                           std::string& error) {
        return registry.registerAction({"test.pending", {}, true},
            [&pending](const GameFlowActionInvocation& invocation) {
                pending = invocation.executionId;
                return GameFlowActionResult::pending();
            }, false, &error);
    };
    auto prepared = prepareGameFlowRuntime(std::move(initial));
    CHECK_NOT_NULL(prepared.get());
    if (prepared == nullptr) return;

    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    CHECK(runtime.initialize());
    runtime.update(0.0f);
    CHECK(pending != 0u);
    CHECK(runtime.snapshot().busy);

    bool candidateConfigured = false;
    auto incompatible = runtimeConfig(candidateConfigured);
    incompatible.documentPath = reloadFixture().string();
    incompatible.enableWorldActions = true;
    const GameFlowReloadResult mismatch = runtime.reload(
        std::move(incompatible));
    CHECK(mismatch.state == GameFlowReloadState::Rejected);
    CHECK_FALSE(runtime.reloadPending());
    CHECK(runtime.snapshot().busy);

    auto candidate = runtimeConfig(candidateConfigured);
    candidate.documentPath = reloadFixture().string();
    const GameFlowReloadResult deferred = runtime.reload(std::move(candidate));
    CHECK(deferred.state == GameFlowReloadState::Deferred);
    CHECK(runtime.reloadPending());
    CHECK(runtime.documentPath() == pendingFixture().string());

    GameFlowRuntimeConfig invalidLatest;
    invalidLatest.documentPath = "__invalid_latest__.gameflow.json";
    invalidLatest.enableWorldActions = false;
    CHECK(runtime.reload(std::move(invalidLatest)).state
        == GameFlowReloadState::Rejected);
    CHECK_FALSE(runtime.reloadPending());

    candidate = runtimeConfig(candidateConfigured);
    candidate.documentPath = reloadFixture().string();
    CHECK(runtime.reload(std::move(candidate)).state
        == GameFlowReloadState::Deferred);
    CHECK(runtime.reloadPending());

    CHECK(runtime.completeAction(
        pending, GameFlowActionResult::succeeded()));
    CHECK(runtime.currentState() == "done");
    runtime.update(0.0f);
    CHECK_FALSE(runtime.reloadPending());
    CHECK(runtime.documentPath() == reloadFixture().string());
    CHECK(runtime.currentState() == "boot");
}

TEST_CASE(reload_requested_inside_guard_is_deferred_until_dispatch_returns)
{
    GameFlowRuntime* runtimePointer = nullptr;
    GameFlowReloadState observed = GameFlowReloadState::Rejected;
    bool guardRan = false;
    bool candidateHandlerRan = false;
    auto candidate = runtimeConfig(candidateHandlerRan);
    candidate.documentPath = reloadFixture().string();

    GameFlowRuntimeConfig initial;
    initial.documentPath = reentrantFixture().string();
    initial.enableWorldActions = false;
    initial.configureRegistry = [&](GameFlowActionRegistry& registry,
                                    std::string& error) {
        return registry.registerGuard(
            {"test.reload.guard", {}},
            [&](const GameFlowGuardInvocation&) {
                guardRan = true;
                const GameFlowReloadResult result = runtimePointer->reload(
                    std::move(candidate));
                observed = result.state;
                return true;
            }, false, &error);
    };

    auto prepared = prepareGameFlowRuntime(std::move(initial));
    CHECK_NOT_NULL(prepared.get());
    if (prepared == nullptr) return;
    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    runtimePointer = &runtime;
    CHECK(runtime.initialize());
    runtime.update(0.0f);

    CHECK(guardRan);
    CHECK(observed == GameFlowReloadState::Deferred);
    CHECK_FALSE(runtime.reloadPending());
    CHECK(runtime.documentPath() == reloadFixture().string());
    CHECK(runtime.currentState() == "boot");
}

TEST_CASE(world_adapter_owned_action_cannot_be_rebound)
{
    bool started = false;
    auto config = runtimeConfig(started);
    config.enableWorldActions = true;
    std::string error;
    auto prepared = prepareGameFlowRuntime(std::move(config), &error);
    CHECK_NOT_NULL(prepared.get());
    CHECK(error.empty());
    if (prepared == nullptr) return;

    RuntimeTestHost host;
    GameFlowRuntime runtime(host, std::move(prepared));
    CHECK_FALSE(runtime.bindActionHandler(kGameFlowActionWorldReplace,
        [](const GameFlowActionInvocation&) {
            return GameFlowActionResult::succeeded();
        }, &error));
    CHECK(error.find("owned") != std::string::npos);
    CHECK_FALSE(runtime.unbindActionHandler(kGameFlowActionWorldReplace));
}

TEST_SUITE_END

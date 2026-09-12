#include <AYApplication/GameFlowRuntime.h>
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
    CHECK_FALSE(runtime.coordinator().busy());

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
    CHECK(runtime.coordinator().currentFlow() == "runtime-child");
    CHECK(runtime.currentState() == "waiting");
    CHECK(runtime.coordinator().callDepth() == 1u);

    CHECK(runtime.request("finish"));
    runtime.update(0.0f);
    CHECK(runtime.coordinator().currentFlow() == "runtime-root");
    CHECK(runtime.currentState() == "ready");
    CHECK(runtime.coordinator().callDepth() == 0u);
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

TEST_SUITE_END

#include <AYApplication/GameFlowRuntime.h>
#include <AYApplication/GameFlowUIBridge.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/UIFlowRuntime.h>
#include <AYEventSystem/EventBus.h>
#include <AYGameLoop.h>
#include <AYTest.h>
#include <AYUI/UIFlow.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using namespace ayt::app;

class BridgeTestHost final : public IEngineHost
{
public:
    ayt::game::IGameLoop& gameLoop() override
    {
        return ayt::game::GameLoop::instance();
    }

    ayt::event::EventBus& eventBus() override { return _events; }
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

    void clearProvidedServices() override { _services.clear(); }
    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override { return nullptr; }
    ayt::physics::IPhysicsQuery* physicsQuery() override { return nullptr; }
    ayt::audio::AudioEngine* audio() override { return nullptr; }
    ayt::scene::SceneManager* scenes() override { return nullptr; }

private:
    ayt::event::EventBus _events;
    std::unordered_map<std::string, void*> _services;
};

class RecordingScreenHost final : public IUIFlowScreenHost
{
public:
    bool mountScreen(
        const UIFlowScreenMountRequest& request,
        std::string&) override
    {
        mounted[request.mountId] = request.screenId;
        return true;
    }

    void unmountScreen(std::uint64_t mountId) noexcept override
    {
        mounted.erase(mountId);
    }

    void setScreenOrder(
        std::uint64_t,
        int,
        std::uint32_t) noexcept override
    {
    }

    std::unordered_map<std::uint64_t, std::string> mounted;
};

std::filesystem::path assetPath(std::string_view name)
{
    return std::filesystem::path(AY_APPLICATION_GAMEFLOW_TEST_ASSET_ROOT)
        / name;
}

bool loadUIFlowFixture(UIFlowRuntime& runtime, std::string& error)
{
    std::ifstream stream(
        assetPath("gameflow_ui.uiflow.json"),
        std::ios::binary);
    if (!stream) {
        error = "Cannot open GameFlow/UIFlow bridge fixture.";
        return false;
    }
    const std::string jsonText{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    ayt::ui::UIFlowDocument document;
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::UIFlowSerializer::deserialize(
            jsonText, document, &diagnostics)) {
        error = diagnostics.empty()
            ? "Cannot deserialize GameFlow/UIFlow bridge fixture."
            : diagnostics.front().path + ": "
                + diagnostics.front().message;
        return false;
    }
    return runtime.load(std::move(document), &error);
}

bool containsScreen(const UIFlowRuntime& runtime, std::string_view id)
{
    for (const UIFlowMountedScreen& screen : runtime.mountedScreens()) {
        if (screen.screenId == id) return true;
    }
    return false;
}

struct BridgeHarness
{
    explicit BridgeHarness(GameFlowUIBridgeConfig config = {})
        : ui(screenHost)
    {
        GameFlowRuntimeConfig runtimeConfig;
        runtimeConfig.documentPath =
            assetPath("gameflow_ui.gameflow.json").string();
        runtimeConfig.enableWorldActions = false;
        runtimeConfig.configureRegistry = [](
            GameFlowActionRegistry& registry,
            std::string& registryError) {
            return registerGameFlowUIActionTypes(
                registry, &registryError);
        };

        auto preparation = prepareGameFlowRuntime(
            std::move(runtimeConfig), &error);
        if (preparation == nullptr) return;
        gameFlow = std::make_unique<GameFlowRuntime>(
            host, std::move(preparation));
        if (!gameFlow->initialize()) {
            error = std::string(gameFlow->lastError());
            return;
        }
        if (!loadUIFlowFixture(ui, error)) return;
        bridge = std::make_unique<GameFlowUIBridge>(
            *gameFlow, ui, std::move(config));
        if (!bridge->install(&error)) return;

        gameFlow->update(0.0f);
        if (gameFlow->currentState() != "ready") {
            error = "GameFlow startup did not enter the ready state.";
            return;
        }
        ready = true;
    }

    BridgeTestHost host;
    RecordingScreenHost screenHost;
    UIFlowRuntime ui;
    std::unique_ptr<GameFlowRuntime> gameFlow;
    std::unique_ptr<GameFlowUIBridge> bridge;
    std::string error;
    bool ready = false;
};

GameFlowUIBridgeConfig menuSignalBinding()
{
    GameFlowUIBridgeConfig config;
    config.signalBindings.push_back({"start_game", "menu.start"});
    return config;
}

GameFlowDocument bridgeSubflow(bool invalidContext = false)
{
    GameFlowDocument child;
    child.id = "runtime-child";
    child.initialState = "waiting";
    child.intents = {{"finish", {}}};
    child.states = {{"waiting"}, {"returned"}, {"failed"}};

    GameFlowTransitionDefinition transition;
    transition.id = "finish_child";
    transition.fromState = "waiting";
    transition.triggerIntent = "finish";
    transition.toState = "returned";
    if (invalidContext) {
        GameFlowPayload arguments;
        arguments["activationId"] = std::string("missing-context-test");
        arguments["contextId"] = std::string("MissingContext");
        transition.actions.push_back({
            std::string(kGameFlowActionUIContextActivate),
            std::move(arguments)});
    }
    transition.actions.push_back({std::string(kGameFlowActionReturn), {}});
    transition.onFailureState = "failed";
    child.transitions.push_back(std::move(transition));
    return child;
}

GameFlowRuntimeConfig bridgeSubflowConfig(bool invalidContext = false)
{
    GameFlowRuntimeConfig config;
    config.documentPath = assetPath("runtime-subflow.gameflow.json").string();
    config.enableWorldActions = false;
    config.configureRegistry = [](
        GameFlowActionRegistry& registry,
        std::string& error) {
        return registerGameFlowUIActionTypes(registry, &error);
    };
    config.resolveDocument = [child = bridgeSubflow(invalidContext)](
        std::string_view id,
        GameFlowDocument& document,
        std::string& error) {
        if (id != child.id) {
            error = "Unknown test subflow.";
            return false;
        }
        document = child;
        error.clear();
        return true;
    };
    return config;
}

} // namespace

TEST_SUITE(GameFlowUIBridgeTests)

TEST_CASE(action_contracts_register_before_runtime_preflight)
{
    GameFlowActionRegistry registry;
    std::string error;
    CHECK(registerGameFlowUIActionTypes(registry, &error));
    CHECK(error.empty());
    CHECK(registry.findAction(kGameFlowActionUIFlowStart) != nullptr);
    CHECK(registry.findAction(kGameFlowActionUIContextActivate) != nullptr);
    CHECK(registry.findAction(kGameFlowActionUIContextDeactivate) != nullptr);
    CHECK(registry.findAction(kGameFlowActionUISignalEmit) != nullptr);
    const GameFlowActionTypeDefinition* start =
        registry.findAction(kGameFlowActionUIFlowStart);
    if (start != nullptr) {
        CHECK(start->arguments.size() == 1u);
        if (start->arguments.size() == 1u) {
            CHECK(start->arguments[0].id == "entry");
            CHECK_FALSE(start->arguments[0].required);
            const auto* defaultEntry = std::get_if<std::string>(
                &start->arguments[0].defaultValue.data);
            CHECK_NOT_NULL(defaultEntry);
            if (defaultEntry != nullptr) CHECK(defaultEntry->empty());
        }
    }

    // Composition helpers may encounter an identical definition contributed
    // by another module; accepting that case keeps setup order independent.
    CHECK(registerGameFlowUIActionTypes(registry, &error));
    CHECK(error.empty());
}

TEST_CASE(bridge_preflight_validates_ui_actions_in_reachable_subflows)
{
    BridgeTestHost host;
    RecordingScreenHost screenHost;
    UIFlowRuntime ui(screenHost);
    std::string error;
    auto preparation = prepareGameFlowRuntime(
        bridgeSubflowConfig(true), &error);
    CHECK_NOT_NULL(preparation);
    if (preparation == nullptr) return;
    GameFlowRuntime gameFlow(host, std::move(preparation));
    CHECK(gameFlow.initialize());
    CHECK(loadUIFlowFixture(ui, error));
    GameFlowUIBridge bridge(gameFlow, ui);
    CHECK_FALSE(bridge.install(&error));
    CHECK(error.find("runtime-child") != std::string::npos);
    CHECK(error.find("MissingContext") != std::string::npos
        || error.find("unknown UIFlow Context") != std::string::npos);
}

TEST_CASE(ui_request_action_resolves_the_active_subflow_intent_contract)
{
    BridgeTestHost host;
    RecordingScreenHost screenHost;
    UIFlowRuntime ui(screenHost);
    std::string error;
    auto preparation = prepareGameFlowRuntime(
        bridgeSubflowConfig(), &error);
    CHECK_NOT_NULL(preparation);
    if (preparation == nullptr) return;
    GameFlowRuntime gameFlow(host, std::move(preparation));
    CHECK(gameFlow.initialize());
    CHECK(loadUIFlowFixture(ui, error));
    GameFlowUIBridge bridge(gameFlow, ui);
    CHECK(bridge.install(&error));
    if (!bridge.installed()) return;

    gameFlow.update(0.0f);
    CHECK(gameFlow.snapshot().currentFlowId == "runtime-child");
    CHECK_NOT_NULL(gameFlow.activeDocument());

    UIFlowPayload inputs;
    inputs["intent"] = std::string("finish");
    inputs["difficulty"] = std::string("normal");
    const UIFlowActionResult accepted = ui.invokeAction(
        kUIFlowActionGameFlowRequest, std::move(inputs));
    CHECK(accepted.accepted);
    gameFlow.update(0.0f);
    CHECK(gameFlow.snapshot().currentFlowId == "runtime-root");
    CHECK(gameFlow.currentState() == "ready");
}

TEST_CASE(ui_signal_binding_resolves_the_active_subflow_intent_contract)
{
    BridgeTestHost host;
    RecordingScreenHost screenHost;
    UIFlowRuntime ui(screenHost);
    std::string error;
    auto preparation = prepareGameFlowRuntime(
        bridgeSubflowConfig(), &error);
    CHECK_NOT_NULL(preparation);
    if (preparation == nullptr) return;
    GameFlowRuntime gameFlow(host, std::move(preparation));
    CHECK(gameFlow.initialize());
    CHECK(loadUIFlowFixture(ui, error));

    GameFlowUIBridgeConfig config;
    config.signalBindings.push_back({"start_game", "finish"});
    GameFlowUIBridge bridge(gameFlow, ui, std::move(config));
    CHECK(bridge.install(&error));
    if (!bridge.installed()) return;
    CHECK(ui.start(std::string_view{}, &error));
    gameFlow.update(0.0f);
    CHECK(gameFlow.snapshot().currentFlowId == "runtime-child");

    UIFlowPayload payload;
    payload["difficulty"] = std::string("normal");
    payload["players"] = std::int64_t(1);
    CHECK(ui.emitSignal("start_game", std::move(payload), &error));
    gameFlow.update(0.0f);

    CHECK(gameFlow.snapshot().currentFlowId == "runtime-root");
    CHECK(gameFlow.currentState() == "ready");
    CHECK(bridge.lastError().empty());
}

TEST_CASE(declared_ui_signal_routes_a_typed_payload_to_gameflow_intent)
{
    BridgeHarness harness(menuSignalBinding());
    CHECK(harness.ready);
    if (!harness.ready) return;
    CHECK(harness.bridge->installed());
    CHECK(harness.ui.isStarted());
    CHECK(containsScreen(harness.ui, "menu"));

    UIFlowPayload payload;
    payload["difficulty"] = std::string("hard");
    payload["players"] = std::int64_t(2);
    std::string error;
    CHECK(harness.ui.emitSignal("start_game", std::move(payload), &error));
    CHECK(error.empty());
    harness.gameFlow->update(0.0f);

    CHECK(harness.gameFlow->currentState() == "playing");
    CHECK(harness.bridge->lastError().empty());
}

TEST_CASE(ui_host_action_requests_gameflow_with_typed_inputs)
{
    BridgeHarness harness;
    CHECK(harness.ready);
    if (!harness.ready) return;

    UIFlowPayload inputs;
    inputs["intent"] = std::string("menu.start");
    inputs["difficulty"] = std::string("normal");
    inputs["players"] = std::int64_t(4);
    const UIFlowActionResult accepted = harness.ui.invokeAction(
        kUIFlowActionGameFlowRequest, std::move(inputs));
    CHECK(accepted.accepted);
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "playing");

    BridgeHarness invalidHarness;
    CHECK(invalidHarness.ready);
    if (!invalidHarness.ready) return;
    UIFlowPayload invalid;
    invalid["intent"] = std::string("menu.start");
    invalid["difficulty"] = std::string("normal");
    invalid["players"] = std::string("four");
    const UIFlowActionResult rejected = invalidHarness.ui.invokeAction(
        kUIFlowActionGameFlowRequest, std::move(invalid));
    CHECK_FALSE(rejected.accepted);
    invalidHarness.gameFlow->update(0.0f);
    CHECK(invalidHarness.gameFlow->currentState() == "ready");
}

TEST_CASE(gameflow_starts_ui_manages_contexts_and_emits_typed_signals)
{
    BridgeHarness harness;
    CHECK(harness.ready);
    if (!harness.ready) return;
    CHECK(harness.ui.isStarted());
    CHECK(containsScreen(harness.ui, "menu"));

    CHECK(harness.gameFlow->request("overlay.show"));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "overlay");
    CHECK(containsScreen(harness.ui, "pause"));
    CHECK_FALSE(containsScreen(harness.ui, "menu"));

    CHECK(harness.gameFlow->request("overlay.hide"));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "ready");
    CHECK(containsScreen(harness.ui, "menu"));
    CHECK_FALSE(containsScreen(harness.ui, "pause"));

    std::size_t notices = 0;
    std::string message;
    std::int64_t code = -1;
    harness.ui.subscribeSignal("bridge.notice",
        [&notices, &message, &code](
            std::string_view,
            const UIFlowPayload& payload) {
            ++notices;
            message = std::get<std::string>(payload.at("message").data);
            code = std::get<std::int64_t>(payload.at("code").data);
        });
    GameFlowPayload payload;
    payload["message"] = std::string("checkpoint saved");
    payload["code"] = std::int64_t(17);
    CHECK(harness.gameFlow->request("ui.notice", std::move(payload)));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "notified");
    CHECK(notices == 1u);
    CHECK(message == "checkpoint saved");
    CHECK(code == 17);
}

TEST_CASE(ui_reload_rejects_bridge_schema_drift_and_keeps_previous_document)
{
    BridgeHarness harness(menuSignalBinding());
    CHECK(harness.ready);
    if (!harness.ready) return;

    ayt::ui::UIFlowDocument candidate = *harness.ui.document();
    candidate.id = "gameflow-ui-host-drift";
    bool changed = false;
    for (auto& signal : candidate.signals) {
        if (signal.id != "start_game") continue;
        for (auto& field : signal.payload) {
            if (field.id != "difficulty") continue;
            field.type = ayt::ui::UIFlowValueType::Integer;
            changed = true;
        }
    }
    CHECK(changed);

    std::string error;
    CHECK_FALSE(harness.ui.reload(std::move(candidate), &error));
    CHECK(error.find("start_game") != std::string::npos);
    CHECK(error.find("difficulty") != std::string::npos);
    CHECK(error.find("incompatible") != std::string::npos);
    CHECK(harness.bridge->installed());
    CHECK(harness.ui.document()->id == "gameflow-ui-host");
    const auto* retained = harness.ui.document()->findSignal("start_game");
    CHECK_NOT_NULL(retained);
    if (retained == nullptr) return;
    CHECK(retained->payload.size() == 2u);
    if (!retained->payload.empty()) {
        CHECK(retained->payload.front().id == "difficulty");
        CHECK(retained->payload.front().type
            == ayt::ui::UIFlowValueType::String);
    }

    UIFlowPayload payload;
    payload["difficulty"] = std::string("hard");
    payload["players"] = std::int64_t(2);
    CHECK(harness.ui.emitSignal("start_game", std::move(payload), &error));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "playing");
}

TEST_CASE(ui_document_validator_cannot_reenter_document_mutation)
{
    RecordingScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    std::string error;
    CHECK(loadUIFlowFixture(runtime, error));
    if (!runtime.isLoaded() || runtime.document() == nullptr) return;

    ayt::ui::UIFlowDocument outer = *runtime.document();
    outer.id = "outer-reload";
    ayt::ui::UIFlowDocument nested = outer;
    nested.id = "nested-reload";
    const std::string outerId = outer.id;

    bool unloadWasIgnored = false;
    bool nestedReloadWasRejected = false;
    std::string nestedError;
    const UIFlowDocumentValidatorToken validator =
        runtime.addDocumentValidator(
            [&](const ayt::ui::UIFlowDocument& candidate,
                std::string& validationError) {
                if (candidate.id != outerId) return true;
                runtime.unload();
                unloadWasIgnored = runtime.isLoaded();
                nestedReloadWasRejected =
                    !runtime.reload(nested, &nestedError)
                    && nestedError.find("already in progress")
                        != std::string::npos;
                if (unloadWasIgnored && nestedReloadWasRejected) return true;
                validationError = "Document mutation reentrancy was not blocked.";
                return false;
            });
    CHECK(validator != 0);

    CHECK(runtime.reload(std::move(outer), &error));
    CHECK(error.empty());
    CHECK(unloadWasIgnored);
    CHECK(nestedReloadWasRejected);
    CHECK(runtime.isLoaded());
    CHECK_NOT_NULL(runtime.document());
    if (runtime.document() != nullptr) {
        CHECK(runtime.document()->id == "outer-reload");
    }
    CHECK(runtime.removeDocumentValidator(validator));
}

TEST_CASE(gameflow_reload_rejects_ui_action_reference_drift_and_keeps_previous_program)
{
    BridgeHarness harness;
    CHECK(harness.ready);
    if (!harness.ready) return;
    const GameFlowProgram* previous = harness.gameFlow->program();
    CHECK_NOT_NULL(previous);
    if (previous == nullptr) return;
    CHECK(previous->rootFlowId == "gameflow-ui-bridge");

    const GameFlowReloadResult result = harness.gameFlow->reload(
        bridgeSubflowConfig(true));
    CHECK(result.state == GameFlowReloadState::Rejected);
    CHECK(result.message.find("runtime-child") != std::string::npos);
    CHECK(result.message.find("finish_child") != std::string::npos);
    CHECK(result.message.find("unknown UIFlow Context")
        != std::string::npos);
    CHECK_FALSE(harness.gameFlow->reloadPending());
    CHECK(harness.gameFlow->program() == previous);
    CHECK(harness.gameFlow->program()->rootFlowId == "gameflow-ui-bridge");
    CHECK(harness.gameFlow->documentPath()
        == assetPath("gameflow_ui.gameflow.json").string());
    CHECK(harness.gameFlow->currentState() == "ready");
    CHECK(harness.bridge->installed());

    CHECK(harness.gameFlow->request("overlay.show"));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "overlay");
    CHECK(containsScreen(harness.ui, "pause"));
}

TEST_CASE(uninstall_removes_both_directions_and_is_idempotent)
{
    BridgeHarness harness(menuSignalBinding());
    CHECK(harness.ready);
    if (!harness.ready) return;

    harness.bridge->uninstall();
    harness.bridge->uninstall();
    CHECK_FALSE(harness.bridge->installed());

    UIFlowPayload inputs;
    inputs["intent"] = std::string("menu.start");
    inputs["difficulty"] = std::string("hard");
    const UIFlowActionResult action = harness.ui.invokeAction(
        kUIFlowActionGameFlowRequest, std::move(inputs));
    CHECK_FALSE(action.accepted);

    UIFlowPayload signalPayload;
    signalPayload["difficulty"] = std::string("hard");
    CHECK(harness.ui.emitSignal("start_game", std::move(signalPayload)));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "ready");

    CHECK(harness.gameFlow->request("overlay.show"));
    harness.gameFlow->update(0.0f);
    CHECK(harness.gameFlow->currentState() == "ui_error");
    CHECK_FALSE(containsScreen(harness.ui, "pause"));
}

TEST_SUITE_END

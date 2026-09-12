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

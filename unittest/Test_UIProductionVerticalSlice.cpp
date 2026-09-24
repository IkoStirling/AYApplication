#include <AYApplication/UIFlowAssetValidation.h>
#include <AYApplication/GameFlowRuntime.h>
#include <AYApplication/GameFlowUIBridge.h>
#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/ProjectContentValidator.h>
#include <AYApplication/RuntimeSceneLoader.h>
#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIFlowSceneBridge.h>
#include <AYApplication/UIManagerFlowScreenHost.h>
#include <AYDevice/DeviceInputEvent.h>
#include <AYTest.h>
#include <AYUI/DeviceInputBridge.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIManager.h>
#include <AYUI/Widget.h>
#include <AYEventSystem/EventBus.h>
#include <AYGameLoop.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

using namespace ayt::app;
using namespace ayt::device;
using namespace ayt::ui;
namespace fs = std::filesystem;

class ProductionHost final : public IEngineHost
{
public:
    explicit ProductionHost(ayt::scene::SceneManager* scenes)
        : _scenes(scenes) {}

    ayt::game::IGameLoop& gameLoop() override {
        return ayt::game::GameLoop::instance();
    }
    ayt::event::EventBus& eventBus() override { return _events; }
    ayt::game::ISubSystem* findSubSystem(const char*) override { return nullptr; }
    void provideService(std::string_view key, void* instance) override {
        if (instance == nullptr) _services.erase(std::string(key));
        else _services[std::string(key)] = instance;
    }
    void* findService(std::string_view key) const override {
        const auto found = _services.find(std::string(key));
        return found == _services.end() ? nullptr : found->second;
    }
    void clearProvidedServices() override { _services.clear(); }
    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override { return nullptr; }
    ayt::physics::IPhysicsQuery* physicsQuery() override { return nullptr; }
    ayt::audio::AudioEngine* audio() override { return nullptr; }
    ayt::scene::SceneManager* scenes() override { return _scenes; }

private:
    ayt::event::EventBus _events;
    ayt::scene::SceneManager* _scenes = nullptr;
    std::unordered_map<std::string, void*> _services;
};

class ProductionWorldRouter final : public IGameWorldRouter
{
public:
    ProductionWorldRouter(IRuntimeSceneLoader& loader,
                          std::vector<GameWorld> worlds,
                          std::string current)
        : _loader(loader), _worlds(std::move(worlds)),
          _current(std::move(current)) {}

    bool requestWorld(std::string_view worldId) override {
        if (_failNext) {
            _failNext = false;
            _lastError = "Injected production recovery failure.";
            return false;
        }
        if (!_pending.empty()) {
            _lastError = "A World transition is already pending.";
            return false;
        }
        if (worldId == _current) {
            _lastError.clear();
            return true;
        }
        const GameWorld* world = findWorld(worldId);
        if (world == nullptr) {
            _lastError = "Unknown World id: " + std::string(worldId);
            return false;
        }
        RuntimeSceneLoadRequest request;
        request.requestId = _nextRequest++;
        request.scenePath = world->scenePath;
        request.sceneName = world->sceneName.empty() ? world->id : world->sceneName;
        request.prepareActivation = world->prepareActivation;
        if (!_loader.requestLoad(std::move(request))) {
            _lastError = "RuntimeSceneLoader rejected the World transition.";
            return false;
        }
        _pendingRequest = _nextRequest - 1u;
        _pending = world->id;
        _lastError.clear();
        return true;
    }

    void update() {
        if (_pending.empty()) return;
        const RuntimeSceneLoadStatus status = _loader.getLoadStatus();
        if (status.requestId != _pendingRequest) return;
        if (status.state == RuntimeSceneLoadState::Ready) {
            _current = std::move(_pending);
            _pending.clear();
            _pendingRequest = 0;
            _lastError.clear();
        } else if (status.state == RuntimeSceneLoadState::Failed
                   || status.state == RuntimeSceneLoadState::Cancelled) {
            _lastError = status.message;
            _pending.clear();
            _pendingRequest = 0;
        }
    }

    void failNextRequest() { _failNext = true; }
    std::string_view currentWorldId() const noexcept override { return _current; }
    std::string_view pendingWorldId() const noexcept override { return _pending; }
    std::string_view lastError() const noexcept override { return _lastError; }
    const GameWorld* findWorld(std::string_view worldId) const noexcept override {
        for (const GameWorld& world : _worlds) {
            if (world.id == worldId) return &world;
        }
        return nullptr;
    }

private:
    IRuntimeSceneLoader& _loader;
    std::vector<GameWorld> _worlds;
    std::string _current;
    std::string _pending;
    std::string _lastError;
    std::uint64_t _nextRequest = 1;
    std::uint64_t _pendingRequest = 0;
    bool _failNext = false;
};

std::string readText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string diagnosticText(const std::vector<UIFlowDiagnostic>& diagnostics)
{
    std::ostringstream result;
    for (const UIFlowDiagnostic& diagnostic : diagnostics) {
        if (result.tellp() > 0) result << " | ";
        result << diagnostic.path << ": " << diagnostic.message;
    }
    return result.str();
}

const UIFlowMountedScreen* mountedScreen(
    const UIFlowRuntime& runtime, std::string_view screenId)
{
    const auto found = std::find_if(
        runtime.mountedScreens().begin(), runtime.mountedScreens().end(),
        [screenId](const UIFlowMountedScreen& screen) {
            return screen.screenId == screenId;
        });
    return found == runtime.mountedScreens().end() ? nullptr : &*found;
}

std::vector<std::string> mountedScreenIds(const UIFlowRuntime& runtime)
{
    std::vector<std::string> result;
    result.reserve(runtime.mountedScreens().size());
    for (const UIFlowMountedScreen& screen : runtime.mountedScreens()) {
        result.push_back(screen.screenId);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string join(const std::vector<std::string>& values)
{
    std::ostringstream result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0u) result << ',';
        result << values[index];
    }
    return result.str();
}

void expectScreens(const UIFlowRuntime& runtime,
                   std::initializer_list<std::string_view> expected,
                   std::string_view step,
                   std::vector<std::string>& failures)
{
    std::vector<std::string> wanted;
    wanted.reserve(expected.size());
    for (const std::string_view id : expected) wanted.emplace_back(id);
    std::sort(wanted.begin(), wanted.end());
    const std::vector<std::string> actual = mountedScreenIds(runtime);
    if (actual != wanted) {
        failures.push_back(std::string(step) + " screens expected ["
            + join(wanted) + "] but got [" + join(actual) + "]");
    }
}

void expectState(const UIFlowRuntime& runtime,
                 std::string_view region,
                 std::string_view expected,
                 std::string_view step,
                 std::vector<std::string>& failures)
{
    const std::string actual(runtime.activeState(region));
    if (actual != expected) {
        failures.push_back(std::string(step) + " region "
            + std::string(region) + " expected " + std::string(expected)
            + " but got " + actual);
    }
}

void addSnapshot(nlohmann::json& trace,
                 std::string_view step,
                 const UIFlowRuntime& runtime,
                 const UIFlowSceneBridge* sceneBridge,
                 const UIManager& manager)
{
    trace["steps"].push_back({
        {"step", step},
        {"screens", mountedScreenIds(runtime)},
        {"regions", {
            {"application", runtime.activeState("application")},
            {"story", runtime.activeState("story")},
            {"notice", runtime.activeState("notice")},
            {"modal", runtime.activeState("modal")},
        }},
        {"world", sceneBridge == nullptr
            ? std::string() : std::string(sceneBridge->currentWorldKey())},
        {"effectiveScale", manager.getEffectiveScale()},
    });
}

bool dispatchClick(DeviceInputBridge& bridge,
                   UIManager& manager,
                   UIManagerFlowScreenHost& host,
                   const UIFlowRuntime& runtime,
                   std::string_view screenId,
                   const std::string& widgetId,
                   std::vector<std::string>& failures)
{
    const UIFlowMountedScreen* screen = mountedScreen(runtime, screenId);
    if (screen == nullptr) {
        failures.push_back("Cannot click " + widgetId + ": Screen "
            + std::string(screenId) + " is not mounted.");
        return false;
    }
    manager.layout();
    Widget* widget = host.findWidget(screen->mountId, widgetId);
    if (widget == nullptr) {
        failures.push_back("Cannot click missing Widget " + widgetId + '.');
        return false;
    }
    const ayt::math::FRectangle bounds = widget->getWorldBounds();
    const ayt::math::FVector2 logical(
        (bounds.minX + bounds.maxX) * 0.5f,
        (bounds.minY + bounds.maxY) * 0.5f);
    const ayt::math::FVector2 physical = manager.logicalToPhysical(logical);

    DeviceInputEvent event{};
    event.type = DeviceInputEventType::MouseMove;
    event.x = physical.x;
    event.y = physical.y;
    (void)bridge.dispatch(event);
    event.type = DeviceInputEventType::MouseButton;
    event.mouseButton = MouseButton::Left;
    event.pressed = true;
    const bool pressed = bridge.dispatch(event);
    event.pressed = false;
    const bool released = bridge.dispatch(event);
    if (!pressed && !released) {
        failures.push_back("AYDevice click was not handled by " + widgetId + '.');
    }
    return pressed || released;
}

void writeTrace(const nlohmann::json& trace,
                std::vector<std::string>& failures)
{
    const fs::path outputRoot(AY_UI_VERTICAL_SLICE_OUTPUT_ROOT);
    std::error_code error;
    fs::create_directories(outputRoot, error);
    if (error) {
        failures.push_back("Could not create vertical-slice output: "
            + error.message());
        return;
    }
    std::ofstream output(outputRoot / "trace.json",
                         std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        failures.push_back("Could not write vertical-slice trace.json.");
        return;
    }
    output << trace.dump(2) << '\n';
}

std::string failureText(const std::vector<std::string>& failures)
{
    std::ostringstream result;
    for (std::size_t index = 0; index < failures.size(); ++index) {
        if (index != 0u) result << " | ";
        result << failures[index];
    }
    return result.str();
}

} // namespace

TEST_SUITE(UIProductionVerticalSlice)

TEST_CASE(asset_driven_menu_scene_parallel_layers_modal_ime_and_dpi)
{
    std::vector<std::string> failures;
    const fs::path assetRoot(AY_UI_VERTICAL_SLICE_ASSET_ROOT);
    const std::string encoded = readText(assetRoot / "production.uiflow.json");
    if (encoded.empty()) {
        CHECK_MSG(false, "Could not read production.uiflow.json");
        return;
    }

    UIFlowDocument document;
    std::vector<UIFlowDiagnostic> diagnostics;
    if (!UIFlowSerializer::deserialize(encoded, document, &diagnostics)) {
        const std::string message = diagnosticText(diagnostics);
        CHECK_MSG(false, message.c_str());
        return;
    }
    const UIFlowAssetValidationResult assetValidation =
        validateUIFlowAssets(document, assetRoot.string());
    if (!assetValidation.valid()) {
        const std::string message = diagnosticText(assetValidation.diagnostics);
        CHECK_MSG(false, message.c_str());
        return;
    }

    UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(1280.0f, 720.0f);
    UIManagerFlowScreenHost host(manager, assetRoot.string());
    UIFlowRuntime runtime(host);
    DeviceInputBridge input(manager);

    int hudClickCount = 0;
    const UIFlowSignalSubscription hudSubscription = runtime.subscribeSignal(
        "hud.clicked",
        [&hudClickCount](std::string_view, const UIFlowPayload&) {
            ++hudClickCount;
        });
    std::string error;
    if (!runtime.load(document, &error) || !runtime.start({}, &error)) {
        CHECK_MSG(false, error.c_str());
        return;
    }

    nlohmann::json trace = {
        {"scenario", "production-ui-vertical-slice"},
        {"viewportDip", {1280, 720}},
        {"input", "AYDevice::DeviceInputEvent via DeviceInputBridge"},
        {"steps", nlohmann::json::array()},
    };

    expectScreens(runtime, {"menu"}, "boot", failures);
    expectState(runtime, "application", "menu", "boot", failures);
    addSnapshot(trace, "boot", runtime, nullptr, manager);

    dispatchClick(input, manager, host, runtime,
                  "menu", "menu_start", failures);
    expectScreens(runtime, {"game"}, "start-game", failures);
    expectState(runtime, "application", "game", "start-game", failures);
    addSnapshot(trace, "start-game", runtime, nullptr, manager);

    ayt::event::EventBus eventBus;
    ayt::scene::Scene town(ayt::scene::SceneMode::Play, "town");
    ayt::scene::Scene arena(ayt::scene::SceneMode::Play, "arena");
    UIFlowSceneBridgeConfig sceneConfig;
    sceneConfig.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    sceneConfig.worldContexts.push_back({"town", "WorldHud"});
    UIFlowSceneBridge sceneBridge(runtime, eventBus, std::move(sceneConfig));
    if (!sceneBridge.start(&town, &error)) {
        CHECK_MSG(false, error.c_str());
        return;
    }
    expectScreens(runtime, {"game", "hud"}, "town", failures);
    addSnapshot(trace, "town", runtime, &sceneBridge, manager);

    if (!sceneBridge.emitSceneSignal("zone.enter", {}, &error)) {
        failures.push_back("zone.enter failed: " + error);
    }
    const UIFlowMountedScreen* firstDialogue = mountedScreen(runtime, "dialogue");
    const std::uint64_t firstDialogueMount = firstDialogue == nullptr
        ? 0u : firstDialogue->mountId;
    if (firstDialogueMount == 0u) {
        failures.push_back("Dialogue did not mount after zone.enter.");
    } else {
        Widget* card = host.findWidget(firstDialogueMount, "dialogue_card");
        if (card == nullptr || card->getOpacity() >= 1.0f) {
            failures.push_back("Dialogue enter animation did not start.");
        }
    }
    host.update(0.05f);
    if (!sceneBridge.emitSceneSignal("zone.exit", {}, &error)) {
        failures.push_back("zone.exit failed: " + error);
    }
    if (firstDialogueMount != 0u
        && !host.isScreenRetiring(firstDialogueMount)) {
        failures.push_back("Interrupted Dialogue did not enter retirement.");
    }
    if (!sceneBridge.emitSceneSignal("zone.enter", {}, &error)) {
        failures.push_back("second zone.enter failed: " + error);
    }
    const UIFlowMountedScreen* secondDialogue = mountedScreen(runtime, "dialogue");
    if (secondDialogue == nullptr
        || secondDialogue->mountId == firstDialogueMount) {
        failures.push_back("Dialogue remount did not receive a fresh mount ID.");
    }
    host.update(0.20f);
    if (firstDialogueMount != 0u
        && host.isScreenRetiring(firstDialogueMount)) {
        failures.push_back("Retired Dialogue survived past its exit duration.");
    }
    expectScreens(runtime, {"dialogue", "game", "hud"},
                  "dialogue-reentered", failures);
    addSnapshot(trace, "dialogue-reentered", runtime, &sceneBridge, manager);

    if (!runtime.emitSignal("notice.show", {}, &error)) {
        failures.push_back("notice.show failed: " + error);
    }
    if (!runtime.emitSignal("pause.open", {}, &error)) {
        failures.push_back("pause.open failed: " + error);
    }
    expectScreens(runtime,
                  {"dialogue", "game", "hud", "notification", "pause"},
                  "parallel-modal", failures);
    expectState(runtime, "modal", "paused", "parallel-modal", failures);
    addSnapshot(trace, "parallel-modal", runtime, &sceneBridge, manager);

    // The top modal must block an otherwise valid click on the lower HUD.
    dispatchClick(input, manager, host, runtime,
                  "hud", "hud_probe", failures);
    if (hudClickCount != 0) {
        failures.push_back("Modal allowed a lower HUD click through.");
    }

    dispatchClick(input, manager, host, runtime,
                  "pause", "pause_name", failures);
    DeviceInputEvent composition{};
    composition.type = DeviceInputEventType::CompositionStart;
    (void)input.dispatch(composition);
    composition.type = DeviceInputEventType::CompositionUpdate;
    composition.text = "ni";
    composition.compositionCursor = 2;
    (void)input.dispatch(composition);
    composition.type = DeviceInputEventType::CompositionEnd;
    composition.text.clear();
    (void)input.dispatch(composition);
    composition.type = DeviceInputEventType::TextCommit;
    composition.text = "\xE4\xBD\xA0";
    (void)input.dispatch(composition);
    const UIFlowMountedScreen* pause = mountedScreen(runtime, "pause");
    auto* nameInput = pause == nullptr ? nullptr : dynamic_cast<TextInput*>(
        host.findWidget(pause->mountId, "pause_name"));
    if (nameInput == nullptr || nameInput->getText() != L"\x4F60") {
        failures.push_back("IME composition/commit did not reach pause_name.");
    }

    dispatchClick(input, manager, host, runtime,
                  "pause", "pause_resume", failures);
    expectState(runtime, "modal", "playing", "resume", failures);
    expectScreens(runtime, {"dialogue", "game", "hud", "notification"},
                  "resume", failures);
    dispatchClick(input, manager, host, runtime,
                  "hud", "hud_probe", failures);
    if (hudClickCount != 1) {
        failures.push_back("HUD click did not recover after Modal closed.");
    }
    addSnapshot(trace, "resume", runtime, &sceneBridge, manager);

    if (!sceneBridge.emitSceneSignal("zone.exit", {}, &error)) {
        failures.push_back("final zone.exit failed: " + error);
    }
    if (!runtime.emitSignal("notice.hide", {}, &error)) {
        failures.push_back("notice.hide failed: " + error);
    }
    host.update(0.20f);
    expectScreens(runtime, {"game", "hud"}, "world-clean", failures);

    if (!sceneBridge.update(&arena, &error)) {
        failures.push_back("Scene replacement failed: " + error);
    }
    expectScreens(runtime, {"game"}, "arena", failures);
    addSnapshot(trace, "arena", runtime, &sceneBridge, manager);

    for (const float dpiScale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        manager.setDpiScale(dpiScale);
        manager.setClientSize(1280.0f * dpiScale, 720.0f * dpiScale);
        host.update(0.0f);
        manager.layout();
        if (manager.getClientSize().x != 1280.0f
            || manager.getClientSize().y != 720.0f) {
            failures.push_back(std::to_string(
                static_cast<int>(dpiScale * 100.0f))
                + "% DPI did not preserve the 1280x720 DIP viewport.");
        }
        if (!runtime.emitSignal("pause.open", {}, &error)) {
            failures.push_back("DPI pause.open failed: " + error);
        }
        dispatchClick(input, manager, host, runtime,
                      "pause", "pause_resume", failures);
        expectScreens(runtime, {"game"}, "dpi-resume", failures);
        addSnapshot(trace,
                    "dpi-resume-" + std::to_string(
                        static_cast<int>(dpiScale * 100.0f)),
                    runtime, &sceneBridge, manager);
    }

    if (!runtime.unsubscribeSignal(hudSubscription)) {
        failures.push_back("HUD Signal subscription could not be removed.");
    }
    writeTrace(trace, failures);
    const std::string message = failureText(failures);
    CHECK_MSG(failures.empty(), message.c_str());

    sceneBridge.stop();
    runtime.unload();
    manager.shutdown();
}

TEST_CASE(real_project_menu_loading_gameplay_pause_result_and_recovery)
{
    const fs::path projectRoot(AY_UI_PRODUCTION_PROJECT_ROOT);
    const fs::path assetRoot = projectRoot / "Assets";
    std::vector<std::string> failures;

    ProjectContentValidationOptions validationOptions;
    validationOptions.enableGameFlowUIActions = true;
    const ProjectContentValidationResult projectValidation =
        validateProjectContent(projectRoot.string(),
            ProjectContentValidationProfile::FullClient,
            std::move(validationOptions));
    if (!projectValidation) {
        for (const ProjectContentValidationIssue& issue : projectValidation.issues) {
            failures.push_back(issue.path + ": " + issue.message);
        }
    }
    if (projectValidation.scenes != 2u) {
        failures.push_back("Packaged project did not close over two Scene assets.");
    }
    if (projectValidation.uiLayouts != 6u) {
        failures.push_back("Packaged project did not close over six UI layouts.");
    }
    if (projectValidation.gameFlows != 1u) {
        failures.push_back("Packaged project did not close over its startup GameFlow.");
    }

    UIFlowDocument uiDocument;
    std::vector<UIFlowDiagnostic> diagnostics;
    if (!UIFlowSerializer::deserialize(
            readText(assetRoot / "ui/production.uiflow.json"),
            uiDocument, &diagnostics)) {
        const std::string message = diagnosticText(diagnostics);
        CHECK_MSG(false, message.c_str());
        return;
    }
    const UIFlowAssetValidationResult uiAssets = validateUIFlowAssets(
        uiDocument, assetRoot.string());
    if (!uiAssets.valid()) {
        for (const UIFlowDiagnostic& diagnostic : uiAssets.diagnostics) {
            failures.push_back(diagnostic.path + ": " + diagnostic.message);
        }
    }
    if (uiAssets.dependencies.size() != 6u) {
        failures.push_back("UI Flow package closure did not contain six layouts.");
    }

    UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(1280.0f, 720.0f);
    UIManagerFlowScreenHost screenHost(manager, assetRoot.string());
    UIFlowRuntime uiRuntime(screenHost);
    DeviceInputBridge input(manager);
    std::string error;
    if (!uiRuntime.load(std::move(uiDocument), &error)) {
        CHECK_MSG(false, error.c_str());
        manager.shutdown();
        return;
    }

    ayt::scene::SceneManager* scenes = defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    RuntimeSceneLoaderConfig loaderConfig;
    loaderConfig.initialSceneName = "Menu";
    loaderConfig.initialScenePath =
        (assetRoot / "worlds/menu.ayscene").string();
    ProductionHost engineHost(scenes);
    std::unique_ptr<IRuntimeSceneLoader> loader = createRuntimeSceneLoader(
        *scenes, std::move(loaderConfig), &engineHost.eventBus());
    if (!loader || !loader->initialize()) {
        CHECK_MSG(false, "Could not initialize production RuntimeSceneLoader.");
        manager.shutdown();
        return;
    }

    std::vector<GameWorld> worlds = {
        {"Menu", (assetRoot / "worlds/menu.ayscene").string(), "Menu"},
        {"Town", (assetRoot / "worlds/town.ayscene").string(), "Town"},
    };
    ProductionWorldRouter router(*loader, std::move(worlds), "Menu");
    engineHost.provideService(kHostServiceRuntimeSceneLoader, loader.get());
    engineHost.provideService(kHostServiceGameWorldRouter, &router);

    GameFlowRuntimeConfig gameConfig;
    gameConfig.documentPath =
        (assetRoot / "flows/production.gameflow.json").string();
    gameConfig.enableWorldActions = true;
    gameConfig.configureRegistry = [](
        GameFlowActionRegistry& registry, std::string& registryError) {
        return registerGameFlowUIActionTypes(registry, &registryError);
    };
    GameFlowRuntime gameRuntime(engineHost, std::move(gameConfig));
    if (!gameRuntime.initialize()) {
        const std::string message(gameRuntime.lastError());
        CHECK_MSG(false, message.c_str());
        loader->shutdown();
        manager.shutdown();
        return;
    }

    GameFlowUIBridgeConfig bridgeConfig;
    bridgeConfig.signalBindings = {
        {"game.start", "menu.start"},
        {"game.startFail", "menu.start"},
        {"loading.cancel", "loading.cancel"},
        {"pause.open", "pause.open"},
        {"pause.close", "pause.close"},
        {"match.finish", "match.finish"},
        {"result.retry", "result.retry"},
        {"result.menu", "result.menu"},
    };
    GameFlowUIBridge gameUiBridge(
        gameRuntime, uiRuntime, std::move(bridgeConfig));
    if (!gameUiBridge.install(&error)) {
        CHECK_MSG(false, error.c_str());
        gameRuntime.shutdown();
        loader->shutdown();
        manager.shutdown();
        return;
    }

    gameRuntime.update(0.0f);
    expectScreens(uiRuntime, {"menu"}, "project-boot", failures);
    if (gameRuntime.currentState() != "menu") {
        failures.push_back("GameFlow did not enter menu after app.start.");
    }

    UIFlowSceneBridgeConfig sceneConfig;
    sceneConfig.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    sceneConfig.worldContexts.push_back({"Town", "WorldHud"});
    UIFlowSceneBridge sceneBridge(
        uiRuntime, engineHost.eventBus(), std::move(sceneConfig));
    if (!sceneBridge.start(loader->currentScene(), &error)) {
        failures.push_back("Scene bridge startup failed: " + error);
    }

    // First exercise a real failed World request and recover through UI input.
    router.failNextRequest();
    dispatchClick(input, manager, screenHost, uiRuntime,
                  "menu", "menu_fail", failures);
    expectScreens(uiRuntime, {"loading"}, "failed-load-visible", failures);
    gameRuntime.update(0.0f);
    if (gameRuntime.currentState() != "load_error") {
        failures.push_back("Rejected World request did not route to load_error.");
    }
    dispatchClick(input, manager, screenHost, uiRuntime,
                  "loading", "loading_back", failures);
    gameRuntime.update(0.0f);
    expectScreens(uiRuntime, {"menu"}, "failed-load-recovered", failures);
    if (gameRuntime.currentState() != "menu") {
        failures.push_back("Loading recovery did not return GameFlow to menu.");
    }

    // Successful path: UI input enters Loading before the asynchronous Scene
    // completion advances both GameFlow and UI Flow to Gameplay.
    dispatchClick(input, manager, screenHost, uiRuntime,
                  "menu", "menu_start", failures);
    expectScreens(uiRuntime, {"loading"}, "loading", failures);
    const UIFlowMountedScreen* loading = mountedScreen(uiRuntime, "loading");
    if (loading != nullptr) {
        Widget* card = screenHost.findWidget(loading->mountId, "loading_card");
        if (card == nullptr || card->getOpacity() >= 1.0f) {
            failures.push_back("Loading enter animation did not start.");
        }
    }
    gameRuntime.update(0.0f);
    if (!gameRuntime.snapshot().busy) {
        failures.push_back("World replacement was not held as a pending action.");
    }
    loader->update(0.0f);
    router.update();
    engineHost.eventBus().pump();
    gameRuntime.update(0.0f);
    if (!sceneBridge.update(loader->currentScene(), &error)) {
        failures.push_back("Town Scene bridge update failed: " + error);
    }
    expectScreens(uiRuntime, {"game", "hud"}, "gameplay", failures);
    if (gameRuntime.currentState() != "gameplay"
        || router.currentWorldId() != "Town") {
        failures.push_back("Successful load did not enter Town gameplay.");
    }

    dispatchClick(input, manager, screenHost, uiRuntime,
                  "hud", "hud_pause", failures);
    gameRuntime.update(0.0f);
    expectScreens(uiRuntime, {"game", "hud", "pause"}, "pause", failures);
    if (gameRuntime.currentState() != "paused") {
        failures.push_back("Pause input did not update GameFlow.");
    }
    dispatchClick(input, manager, screenHost, uiRuntime,
                  "pause", "pause_resume", failures);
    gameRuntime.update(0.0f);
    expectScreens(uiRuntime, {"game", "hud"}, "resume", failures);

    dispatchClick(input, manager, screenHost, uiRuntime,
                  "hud", "hud_finish", failures);
    gameRuntime.update(0.0f);
    expectScreens(uiRuntime, {"result"}, "result", failures);
    if (gameRuntime.currentState() != "result") {
        failures.push_back("Finish input did not enter the Result state.");
    }
    const UIFlowMountedScreen* result = mountedScreen(uiRuntime, "result");
    if (result != nullptr) {
        Widget* card = screenHost.findWidget(result->mountId, "result_card");
        if (card == nullptr || card->getOpacity() >= 1.0f) {
            failures.push_back("Result enter animation did not start.");
        }
    }

    dispatchClick(input, manager, screenHost, uiRuntime,
                  "result", "result_retry", failures);
    gameRuntime.update(0.0f);
    expectScreens(uiRuntime, {"game", "hud"}, "retry", failures);
    if (gameRuntime.currentState() != "gameplay") {
        failures.push_back("Retry did not return GameFlow to gameplay.");
    }

    dispatchClick(input, manager, screenHost, uiRuntime,
                  "hud", "hud_finish", failures);
    gameRuntime.update(0.0f);
    dispatchClick(input, manager, screenHost, uiRuntime,
                  "result", "result_menu", failures);
    gameRuntime.update(0.0f);
    loader->update(0.0f);
    router.update();
    engineHost.eventBus().pump();
    gameRuntime.update(0.0f);
    if (!sceneBridge.update(loader->currentScene(), &error)) {
        failures.push_back("Menu Scene bridge update failed: " + error);
    }
    expectScreens(uiRuntime, {"menu"}, "return-menu", failures);
    if (gameRuntime.currentState() != "menu"
        || router.currentWorldId() != "Menu") {
        failures.push_back("Result-to-Menu did not complete its World switch.");
    }

    const std::string message = failureText(failures);
    CHECK_MSG(failures.empty(), message.c_str());

    sceneBridge.stop();
    gameUiBridge.uninstall();
    gameRuntime.shutdown();
    loader->shutdown();
    uiRuntime.unload();
    manager.shutdown();
    scenes->setCurrent(nullptr);
}

TEST_SUITE_END

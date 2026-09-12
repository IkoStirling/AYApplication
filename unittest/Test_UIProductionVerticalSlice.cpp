#include <AYApplication/UIFlowAssetValidation.h>
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
#include <AYScene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace ayt::app;
using namespace ayt::device;
using namespace ayt::ui;
namespace fs = std::filesystem;

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

    manager.setDpiScale(1.5f);
    manager.setClientSize(1920.0f, 1080.0f);
    host.update(0.0f);
    manager.layout();
    if (manager.getClientSize().x != 1280.0f
        || manager.getClientSize().y != 720.0f) {
        failures.push_back("150% DPI did not preserve the 1280x720 DIP viewport.");
    }
    if (!runtime.emitSignal("pause.open", {}, &error)) {
        failures.push_back("DPI pause.open failed: " + error);
    }
    dispatchClick(input, manager, host, runtime,
                  "pause", "pause_resume", failures);
    expectScreens(runtime, {"game"}, "dpi-resume", failures);
    addSnapshot(trace, "dpi-resume", runtime, &sceneBridge, manager);

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

TEST_SUITE_END

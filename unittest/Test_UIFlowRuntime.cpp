#include <AYApplication/UIFlowAssetValidation.h>
#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIManagerFlowScreenHost.h>
#include <AYTest.h>
#include <AYUI/Animation.h>
#include <AYUI/Button.h>
#include <AYUI/UIManager.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using namespace ayt::app;
using namespace ayt::ui;

class RecordingScreenHost final : public IUIFlowScreenHost
{
public:
    bool mountScreen(
        const UIFlowScreenMountRequest& request,
        std::string& error) override
    {
        events.push_back("mount:" + request.screenId);
        if (request.screenId == throwingScreen) {
            throw std::runtime_error("deliberate host exception");
        }
        if (request.screenId == rejectedScreen) {
            error = "deliberate test rejection";
            return false;
        }
        active[request.mountId] = request;
        return true;
    }

    void unmountScreen(std::uint64_t mountId) noexcept override
    {
        const auto found = active.find(mountId);
        events.push_back(found == active.end()
            ? "unmount:unknown"
            : "unmount:" + found->second.screenId);
        active.erase(mountId);
    }

    void setScreenOrder(
        std::uint64_t mountId,
        int layerOrder,
        std::uint32_t orderInLayer) noexcept override
    {
        const auto found = active.find(mountId);
        if (found == active.end()) return;
        found->second.layerOrder = layerOrder;
        found->second.orderInLayer = orderInLayer;
    }

    std::string rejectedScreen;
    std::string throwingScreen;
    std::unordered_map<std::uint64_t, UIFlowScreenMountRequest> active;
    std::vector<std::string> events;
};

class MouseProbeWidget final : public ayt::ui::Widget
{
public:
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent&) override
    {
        pressed = true;
        return true;
    }

    bool pressed = false;
};

UIFlowDocument arbitrationDocument(bool restorePrevious = true)
{
    UIFlowDocument document;
    document.id = "runtime-arbitration";
    document.defaultEntry = "boot";
    document.layers.push_back(UIFlowLayerDefinition{
        "main", 10, UIFlowInputPolicy::ConsumeHandled, false, 1});
    document.slots.push_back(UIFlowSlotDefinition{
        "main.content", "main", 1, restorePrevious});
    document.screens.push_back(UIFlowScreenDefinition{
        "menu", "menu.ui.json", "main", "main.content",
        UIFlowScope::Application});
    document.screens.push_back(UIFlowScreenDefinition{
        "pause", "pause.ui.json", "main", "main.content",
        UIFlowScope::Application});
    document.contexts.push_back(UIFlowContextDefinition{
        "Menu", 0, {{"main.content", UIFlowSlotOperation::Present, "menu"}}});
    document.contexts.push_back(UIFlowContextDefinition{
        "Pause", 100,
        {{"main.content", UIFlowSlotOperation::Present, "pause"}}});
    document.entries.push_back(UIFlowEntryDefinition{"boot", {"Menu"}, {}});
    return document;
}

UIFlowDocument scopedDocument()
{
    UIFlowDocument document;
    document.id = "runtime-scopes";
    document.defaultEntry = "boot";
    document.layers = {
        UIFlowLayerDefinition{"app", 0},
        UIFlowLayerDefinition{"hud", 100},
        UIFlowLayerDefinition{"owner", 200},
    };
    document.slots = {
        UIFlowSlotDefinition{"app.main", "app", 1, true},
        UIFlowSlotDefinition{"hud.main", "hud", 1, true},
        UIFlowSlotDefinition{"owner.main", "owner", 1, true},
    };
    document.screens = {
        UIFlowScreenDefinition{
            "shell", "shell.ui.json", "app", "app.main",
            UIFlowScope::Application},
        UIFlowScreenDefinition{
            "hud", "hud.ui.json", "hud", "hud.main",
            UIFlowScope::World},
        UIFlowScreenDefinition{
            "prompt", "prompt.ui.json", "owner", "owner.main",
            UIFlowScope::Owner},
    };
    document.contexts = {
        UIFlowContextDefinition{
            "Boot", 0,
            {
                {"app.main", UIFlowSlotOperation::Present, "shell"},
                {"hud.main", UIFlowSlotOperation::Present, "hud"},
            }},
        UIFlowContextDefinition{
            "Prompt", 0,
            {{"owner.main", UIFlowSlotOperation::Present, "prompt"}}},
    };
    document.entries.push_back(UIFlowEntryDefinition{"boot", {"Boot"}, {}});
    return document;
}

UIFlowDocument stateDocument()
{
    UIFlowDocument document;
    document.id = "runtime-states";
    document.layers = {
        UIFlowLayerDefinition{"application", 0},
        UIFlowLayerDefinition{"story", 100},
    };
    document.slots = {
        UIFlowSlotDefinition{"application.main", "application", 1, true},
        UIFlowSlotDefinition{"story.main", "story", 1, true},
    };
    document.screens = {
        UIFlowScreenDefinition{
            "menu", "menu.ui.json", "application", "application.main",
            UIFlowScope::Application},
        UIFlowScreenDefinition{
            "game", "game.ui.json", "application", "application.main",
            UIFlowScope::Application},
        UIFlowScreenDefinition{
            "subtitle", "subtitle.ui.json", "story", "story.main",
            UIFlowScope::Application},
    };
    document.contexts = {
        UIFlowContextDefinition{
            "Menu", 0,
            {{"application.main", UIFlowSlotOperation::Present, "menu"}}},
        UIFlowContextDefinition{
            "Game", 0,
            {{"application.main", UIFlowSlotOperation::Present, "game"}}},
        UIFlowContextDefinition{
            "Subtitle", 0,
            {{"story.main", UIFlowSlotOperation::Present, "subtitle"}}},
    };
    document.signals = {
        UIFlowSignalDefinition{
            "start",
            {
                UIFlowFieldDefinition{
                    "allowed", UIFlowValueType::Boolean, true, {}},
                UIFlowFieldDefinition{
                    "fade", UIFlowValueType::Number, false, 0.25},
            }},
        UIFlowSignalDefinition{"after", {}},
    };
    document.graphs = {
        UIFlowGraphDefinition{"leave_menu"},
        UIFlowGraphDefinition{"start_game"},
        UIFlowGraphDefinition{"enter_game"},
        UIFlowGraphDefinition{"show_story"},
    };
    document.regions = {
        UIFlowRegionDefinition{
            "application", "menu",
            {
                UIFlowStateDefinition{"menu", {}, {}, {"Menu"}, {}, "leave_menu"},
                UIFlowStateDefinition{"game", {}, {}, {"Game"}, "enter_game", {}},
            }},
        UIFlowRegionDefinition{
            "story", "idle",
            {
                UIFlowStateDefinition{"idle"},
                UIFlowStateDefinition{"visible", {}, {}, {"Subtitle"}},
            }},
    };
    document.transitions = {
        UIFlowTransitionDefinition{
            "application.start", "application", "menu", "game", "start",
            "payload.allowed", "start_game", 10},
        UIFlowTransitionDefinition{
            "story.start", "story", "idle", "visible", "start",
            {}, "show_story", 0},
    };
    document.actions.push_back(UIFlowActionDefinition{
        "world.load",
        {
            UIFlowFieldDefinition{
                "world", UIFlowValueType::String, true, {}},
            UIFlowFieldDefinition{
                "fade", UIFlowValueType::Number, false, 0.5},
        }});
    return document;
}

bool containsScreen(
    const std::vector<UIFlowMountedScreen>& screens,
    const std::string& id)
{
    return std::any_of(screens.begin(), screens.end(),
        [&id](const UIFlowMountedScreen& screen) {
            return screen.screenId == id;
        });
}

UIFlowDocument asynchronousDocument(UIFlowInterruptPolicy nextPolicy)
{
    UIFlowDocument document;
    document.id = "async-runtime";
    document.signals = {
        UIFlowSignalDefinition{"go", {}},
        UIFlowSignalDefinition{"next", {}},
        UIFlowSignalDefinition{"skip", {}},
    };
    document.graphs = {
        UIFlowGraphDefinition{"graph.go"},
        UIFlowGraphDefinition{"graph.next"},
        UIFlowGraphDefinition{"graph.skip"},
    };
    document.regions.push_back(UIFlowRegionDefinition{
        "main", "a", {
            UIFlowStateDefinition{"a"},
            UIFlowStateDefinition{"b"},
            UIFlowStateDefinition{"c"},
            UIFlowStateDefinition{"d"},
        }});
    document.transitions = {
        UIFlowTransitionDefinition{
            "go", "main", "a", "b", "go", {}, "graph.go", 0,
            UIFlowInterruptPolicy::Queue},
        UIFlowTransitionDefinition{
            "next", "main", "b", "c", "next", {}, "graph.next", 0,
            nextPolicy},
        UIFlowTransitionDefinition{
            "skip", "main", "b", "d", "skip", {}, "graph.skip", 0,
            nextPolicy},
    };
    return document;
}

} // namespace

TEST_SUITE(UIFlowRuntimeTests)

TEST_CASE(context_priority_replaces_and_restores_screen_atomically)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    std::string error;
    CHECK(runtime.load(arbitrationDocument(), &error));
    CHECK(runtime.start({}, &error));
    CHECK(runtime.mountedScreens().size() == 1u);
    CHECK(runtime.mountedScreens()[0].screenId == "menu");

    host.events.clear();
    const UIFlowContextHandle pause = runtime.activateContext("Pause", {}, &error);
    CHECK(pause != 0u);
    CHECK(runtime.mountedScreens()[0].screenId == "pause");
    CHECK(host.events.size() == 2u);
    CHECK(host.events[0] == "mount:pause");
    CHECK(host.events[1] == "unmount:menu");

    host.events.clear();
    CHECK(runtime.deactivateContext(pause, &error));
    CHECK(runtime.mountedScreens()[0].screenId == "menu");
    CHECK(host.events.size() == 2u);
    CHECK(host.events[0] == "mount:menu");
    CHECK(host.events[1] == "unmount:pause");
}

TEST_CASE(non_restoring_slot_suppresses_old_activation_until_reactivated)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(arbitrationDocument(false)));
    CHECK(runtime.start());
    const UIFlowContextHandle pause = runtime.activateContext("Pause");
    CHECK(pause != 0u);
    CHECK(runtime.deactivateContext(pause));
    CHECK(runtime.mountedScreens().empty());

    const UIFlowContextHandle menu = runtime.activateContext("Menu");
    CHECK(menu != 0u);
    CHECK(runtime.mountedScreens().size() == 1u);
    CHECK(runtime.mountedScreens()[0].screenId == "menu");
}

TEST_CASE(slot_capacity_stacks_candidates_and_high_priority_hide_suppresses_all)
{
    UIFlowDocument document = arbitrationDocument();
    document.layers[0].maxActiveScreens = 0;
    document.slots[0].capacity = 2;
    document.contexts.push_back(UIFlowContextDefinition{
        "HideAll", 200,
        {{"main.content", UIFlowSlotOperation::Hide, {}}}});

    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(std::move(document)));
    CHECK(runtime.start());
    const UIFlowContextHandle pause = runtime.activateContext("Pause");
    CHECK(pause != 0u);
    CHECK(runtime.mountedScreens().size() == 2u);
    CHECK(runtime.mountedScreens()[0].screenId == "menu");
    CHECK(runtime.mountedScreens()[1].screenId == "pause");
    CHECK(runtime.mountedScreens()[0].orderInLayer == 0u);
    CHECK(runtime.mountedScreens()[1].orderInLayer == 1u);

    const UIFlowContextHandle hide = runtime.activateContext("HideAll");
    CHECK(hide != 0u);
    CHECK(runtime.mountedScreens().empty());
    CHECK(runtime.deactivateContext(hide));
    CHECK(runtime.mountedScreens().size() == 2u);
}

TEST_CASE(world_and_owner_scopes_mount_and_cleanup_independently)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(scopedDocument()));
    CHECK(runtime.start());
    CHECK(runtime.mountedScreens().size() == 1u);
    CHECK(containsScreen(runtime.mountedScreens(), "shell"));

    CHECK(runtime.beginScope(UIFlowScope::World, "world:level_a"));
    CHECK(runtime.mountedScreens().size() == 2u);
    CHECK(containsScreen(runtime.mountedScreens(), "hud"));

    CHECK(runtime.beginScope(UIFlowScope::Owner, "entity:42"));
    UIFlowContextActivationOptions ownerOptions;
    ownerOptions.lifetime.scope = UIFlowScope::Owner;
    const UIFlowContextHandle prompt = runtime.activateContext(
        "Prompt", ownerOptions);
    CHECK(prompt != 0u);
    CHECK(containsScreen(runtime.mountedScreens(), "prompt"));

    CHECK(runtime.endScope(UIFlowScope::Owner, "entity:42"));
    CHECK_FALSE(containsScreen(runtime.mountedScreens(), "prompt"));
    CHECK(containsScreen(runtime.mountedScreens(), "hud"));
    CHECK(runtime.endScope(UIFlowScope::World, "world:level_a"));
    CHECK(runtime.mountedScreens().size() == 1u);
    CHECK(runtime.mountedScreens()[0].screenId == "shell");
    CHECK_FALSE(runtime.endScope(UIFlowScope::Application, "application"));
}

TEST_CASE(signal_drives_parallel_regions_graph_requests_and_reentrant_delivery)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    std::vector<std::string> graphs;
    std::vector<std::string> observed;
    runtime.setGuardEvaluator([](
        std::string_view expression,
        const UIFlowPayload& payload,
        std::string& error) {
        error.clear();
        return expression == "payload.allowed"
            && std::get<bool>(payload.at("allowed").data);
    });
    runtime.setGraphRequestHandler(
        [&graphs](const UIFlowGraphRequest& request) {
            graphs.push_back(request.graphId);
        });
    runtime.subscribeSignal("*",
        [&runtime, &observed](std::string_view id, const UIFlowPayload&) {
            observed.emplace_back(id);
            if (id == "start") CHECK(runtime.emitSignal("after"));
        });

    CHECK(runtime.load(stateDocument()));
    CHECK(runtime.start());
    graphs.clear();
    UIFlowPayload payload;
    payload["allowed"] = true;
    CHECK(runtime.emitSignal("start", std::move(payload)));

    CHECK(runtime.activeState("application") == "game");
    CHECK(runtime.activeState("story") == "visible");
    CHECK(containsScreen(runtime.mountedScreens(), "game"));
    CHECK(containsScreen(runtime.mountedScreens(), "subtitle"));
    CHECK(graphs.size() == 4u);
    CHECK(graphs[0] == "leave_menu");
    CHECK(graphs[1] == "start_game");
    CHECK(graphs[2] == "enter_game");
    CHECK(graphs[3] == "show_story");
    CHECK(observed.size() == 2u);
    CHECK(observed[0] == "start");
    CHECK(observed[1] == "after");
}

TEST_CASE(sibling_transition_preserves_common_parent_context_and_transient_screen)
{
    UIFlowDocument document;
    document.id = "hierarchical-state";
    document.layers.push_back(UIFlowLayerDefinition{"main", 0});
    document.slots = {
        UIFlowSlotDefinition{"main.parent", "main", 1, true},
        UIFlowSlotDefinition{"main.child", "main", 1, true},
    };
    document.screens = {
        UIFlowScreenDefinition{
            "parent", "parent.ui.json", "main", "main.parent",
            UIFlowScope::Transient},
        UIFlowScreenDefinition{
            "child-a", "a.ui.json", "main", "main.child",
            UIFlowScope::Application},
        UIFlowScreenDefinition{
            "child-b", "b.ui.json", "main", "main.child",
            UIFlowScope::Application},
    };
    document.contexts = {
        UIFlowContextDefinition{
            "Parent", 0,
            {{"main.parent", UIFlowSlotOperation::Present, "parent"}}},
        UIFlowContextDefinition{
            "ChildA", 0,
            {{"main.child", UIFlowSlotOperation::Present, "child-a"}}},
        UIFlowContextDefinition{
            "ChildB", 0,
            {{"main.child", UIFlowSlotOperation::Present, "child-b"}}},
    };
    document.signals.push_back(UIFlowSignalDefinition{"next", {}});
    document.regions.push_back(UIFlowRegionDefinition{
        "page", "root",
        {
            UIFlowStateDefinition{"root", {}, "a", {"Parent"}},
            UIFlowStateDefinition{"a", "root", {}, {"ChildA"}},
            UIFlowStateDefinition{"b", "root", {}, {"ChildB"}},
        }});
    document.transitions.push_back(UIFlowTransitionDefinition{
        "next", "page", "a", "b", "next"});

    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(std::move(document)));
    CHECK(runtime.start());
    const auto parent = std::find_if(
        runtime.mountedScreens().begin(), runtime.mountedScreens().end(),
        [](const UIFlowMountedScreen& screen) {
            return screen.screenId == "parent";
        });
    CHECK(parent != runtime.mountedScreens().end());
    const std::uint64_t parentMountId = parent->mountId;

    host.events.clear();
    CHECK(runtime.emitSignal("next"));
    const auto preserved = std::find_if(
        runtime.mountedScreens().begin(), runtime.mountedScreens().end(),
        [](const UIFlowMountedScreen& screen) {
            return screen.screenId == "parent";
        });
    CHECK(preserved != runtime.mountedScreens().end());
    CHECK(preserved->mountId == parentMountId);
    CHECK(std::find(host.events.begin(), host.events.end(), "unmount:parent")
          == host.events.end());
    CHECK(containsScreen(runtime.mountedScreens(), "child-b"));
}

TEST_CASE(action_registry_validates_inputs_and_applies_defaults)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(stateDocument()));
    bool called = false;
    CHECK(runtime.registerAction("world.load",
        [&called](const UIFlowActionInvocation& invocation) {
            called = true;
            CHECK(std::get<std::string>(
                invocation.inputs.at("world").data) == "arena");
            CHECK(std::get<double>(
                invocation.inputs.at("fade").data) == 0.5);
            return UIFlowActionResult::success();
        }));

    UIFlowActionResult missing = runtime.invokeAction("world.load");
    CHECK_FALSE(missing.accepted);
    CHECK_FALSE(called);
    UIFlowPayload inputs;
    inputs["world"] = "arena";
    const UIFlowActionResult accepted = runtime.invokeAction(
        "world.load", std::move(inputs));
    CHECK(accepted.accepted);
    CHECK(called);
}

TEST_CASE(mount_failure_preserves_previous_presentation)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(arbitrationDocument()));
    CHECK(runtime.start());
    const std::uint64_t oldMountId = runtime.mountedScreens()[0].mountId;
    host.rejectedScreen = "pause";
    std::string error;
    CHECK(runtime.activateContext("Pause", {}, &error) == 0u);
    CHECK(error.find("deliberate test rejection") != std::string::npos);
    CHECK(runtime.mountedScreens().size() == 1u);
    CHECK(runtime.mountedScreens()[0].mountId == oldMountId);
    CHECK(runtime.mountedScreens()[0].screenId == "menu");
    CHECK(host.active.size() == 1u);

    CHECK_FALSE(runtime.start("missing", &error));
    CHECK(runtime.isStarted());
    CHECK(runtime.mountedScreens()[0].screenId == "menu");

    host.rejectedScreen.clear();
    host.throwingScreen = "pause";
    CHECK(runtime.activateContext("Pause", {}, &error) == 0u);
    CHECK(error.find("deliberate host exception") != std::string::npos);
    CHECK(runtime.mountedScreens()[0].mountId == oldMountId);
}

TEST_CASE(external_callback_exceptions_are_contained_and_dispatch_recovers)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(stateDocument()));
    CHECK(runtime.start());
    runtime.setGuardEvaluator([](
        std::string_view,
        const UIFlowPayload&,
        std::string&) -> bool {
        throw std::runtime_error("guard fault");
    });
    UIFlowPayload payload;
    payload["allowed"] = true;
    std::string error;
    CHECK_FALSE(runtime.emitSignal("start", payload, &error));
    CHECK(error.find("guard fault") != std::string::npos);
    CHECK(runtime.activeState("application") == "menu");

    runtime.setGuardEvaluator([](
        std::string_view,
        const UIFlowPayload&,
        std::string& callbackError) {
        callbackError.clear();
        return true;
    });
    runtime.setGraphRequestHandler([](const UIFlowGraphRequest& request) {
        if (request.graphId == "start_game") {
            throw std::runtime_error("graph fault");
        }
    });
    CHECK_FALSE(runtime.emitSignal("start", payload, &error));
    CHECK(error.find("graph fault") != std::string::npos);
    // Presentation commits before asynchronous graph requests are issued.
    CHECK(runtime.activeState("application") == "game");
    CHECK(runtime.emitSignal("after", {}, &error));
    CHECK(error.empty());
}

TEST_CASE(move_assignment_unmounts_the_replaced_runtime)
{
    RecordingScreenHost firstHost;
    RecordingScreenHost secondHost;
    UIFlowRuntime first(firstHost);
    UIFlowRuntime second(secondHost);
    CHECK(first.load(arbitrationDocument()));
    CHECK(first.start());
    CHECK(second.load(arbitrationDocument()));
    CHECK(second.start());
    CHECK(firstHost.active.size() == 1u);
    CHECK(secondHost.active.size() == 1u);

    first = std::move(second);
    CHECK(firstHost.active.empty());
    CHECK(first.mountedScreens().size() == 1u);
    CHECK(secondHost.active.size() == 1u);
}

TEST_CASE(ui_manager_host_mounts_independent_layout_and_widget_registry)
{
    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(800.0f, 450.0f);

    UIManagerFlowScreenHost host(manager, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    UIFlowScreenMountRequest request;
    request.mountId = 17;
    request.screenId = "fixture";
    request.layoutAsset = "ui_flow_screen.ui.json";
    request.layerId = "main";
    request.slotId = "main.content";
    std::string error;
    CHECK(host.mountScreen(request, error));
    CHECK(error.empty());
    CHECK(host.screenRoot(17) != nullptr);
    CHECK(host.screenRoot(17)->getSize().x == 800.0f);
    CHECK(host.screenRoot(17)->getSize().y == 450.0f);
    CHECK(host.findWidget(17, "flow_fixture_button") != nullptr);

    host.unmountScreen(17);
    CHECK(host.screenRoot(17) == nullptr);
    CHECK(host.findWidget(17, "flow_fixture_button") == nullptr);
}

TEST_CASE(widget_declarative_event_emits_declared_flow_signal)
{
    UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(640.0f, 360.0f);
    UIManagerFlowScreenHost host(
        manager, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    UIFlowRuntime runtime(host);
    UIFlowDocument document = arbitrationDocument();
    document.screens[0].layoutAsset = "ui_flow_screen.ui.json";
    document.screens[0].events.push_back(
        {"continueFlow", "ui.continue"});
    document.signals.push_back({"ui.continue", {}});
    int emissions = 0;
    CHECK(runtime.load(std::move(document)));
    CHECK(runtime.subscribeSignal("ui.continue",
        [&emissions](std::string_view, const UIFlowPayload&) {
            ++emissions;
        }) != 0u);
    CHECK(runtime.start());
    auto* button = dynamic_cast<Button*>(
        host.findWidget(runtime.mountedScreens().front().mountId,
                        "flow_fixture_button"));
    CHECK(button != nullptr);
    if (button != nullptr) {
        const auto bounds = button->getWorldBounds();
        const UIMouseEvent click({(bounds.minX + bounds.maxX) * 0.5f,
                                  (bounds.minY + bounds.maxY) * 0.5f}, 0);
        CHECK(button->onMouseMove(click));
        CHECK(button->onMouseButtonDown(click));
        CHECK(button->onMouseButtonUp(click));
    }
    CHECK(emissions == 1);
    CHECK(runtime.replaySignals().size() == 1u);
    CHECK(runtime.replaySignals().front().signalId == "ui.continue");
}

TEST_CASE(ui_manager_pass_through_layer_ignores_blank_screen_surface)
{
    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(800.0f, 450.0f);
    MouseProbeWidget lower;
    lower.setPosition(ayt::math::FVector2(600.0f, 340.0f));
    lower.setSize(ayt::math::FVector2(120.0f, 80.0f));
    manager.root()->addChildExternal(&lower);

    UIManagerFlowScreenHost host(manager, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    UIFlowScreenMountRequest request;
    request.mountId = 18;
    request.screenId = "pass-through";
    request.layoutAsset = "ui_flow_screen.ui.json";
    request.layerId = "hud";
    request.slotId = "hud.main";
    request.inputPolicy = UIFlowInputPolicy::PassThrough;
    std::string error;
    CHECK(host.mountScreen(request, error));
    CHECK(manager.onMouseButtonDown(640.0f, 380.0f, 0));
    CHECK(lower.pressed);
}

TEST_CASE(ui_manager_root_replacement_does_not_double_destroy_flow_widgets)
{
    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(800.0f, 450.0f);
    {
        UIManagerFlowScreenHost host(
            manager, AY_APPLICATION_UI_TEST_ASSET_ROOT);
        UIFlowScreenMountRequest request;
        request.mountId = 19;
        request.screenId = "replace-root";
        request.layoutAsset = "ui_flow_screen.ui.json";
        request.layerId = "main";
        request.slotId = "main.content";
        std::string error;
        CHECK(host.mountScreen(request, error));
        ayt::ui::Widget* mountedRoot = host.screenRoot(19);
        CHECK(mountedRoot != nullptr);
        if (mountedRoot == nullptr) return;
        CHECK(manager.loadFromString(
            R"json({"type":"Panel","id":"replacement"})json"));
        CHECK(host.screenRoot(19) == mountedRoot);
        host.update(0.0f);
        ayt::ui::Widget* treeRoot = mountedRoot;
        while (treeRoot->getParent() != nullptr) {
            treeRoot = treeRoot->getParent();
        }
        CHECK(treeRoot == manager.root());
        host.unmountScreen(19);
        CHECK(host.screenRoot(19) == nullptr);
    }
    CHECK(manager.root() != nullptr);
    CHECK(manager.root()->getId() == "replacement");
}

TEST_CASE(async_graph_queue_defers_transition_until_completion)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    std::vector<UIFlowGraphExecutionId> executions;
    runtime.setAsyncGraphRequestHandler(
        [&executions](const UIFlowGraphExecutionRequest& request) {
            executions.push_back(request.executionId);
            return UIFlowGraphStartResult::running();
        });
    CHECK(runtime.load(asynchronousDocument(UIFlowInterruptPolicy::Queue)));
    CHECK(runtime.start());
    CHECK(runtime.emitSignal("go"));
    CHECK(runtime.activeState("main") == "b");
    CHECK(runtime.hasPendingGraphExecution("main"));
    CHECK(runtime.emitSignal("next"));
    CHECK(runtime.activeState("main") == "b");
    CHECK(executions.size() == 1u);
    std::string reloadError;
    CHECK_FALSE(runtime.reload(
        asynchronousDocument(UIFlowInterruptPolicy::Queue), &reloadError));
    CHECK(reloadError.find("asynchronous graph") != std::string::npos);
    if (executions.size() != 1u) return;
    CHECK(runtime.completeGraphExecution(executions[0]));
    CHECK(runtime.activeState("main") == "c");
    CHECK(runtime.hasPendingGraphExecution("main"));
    CHECK(executions.size() == 2u);
    if (executions.size() != 2u) return;
    CHECK(runtime.completeGraphExecution(executions[1]));
    CHECK_FALSE(runtime.hasPendingGraphExecution());
}

TEST_CASE(async_graph_coalesce_keeps_latest_and_ignore_drops_deferred_transition)
{
    for (const UIFlowInterruptPolicy policy : {
             UIFlowInterruptPolicy::Coalesce,
             UIFlowInterruptPolicy::IgnoreIfRunning}) {
        RecordingScreenHost host;
        std::vector<UIFlowGraphExecutionId> executions;
        UIFlowRuntime runtime(host);
        runtime.setAsyncGraphRequestHandler(
            [&](const UIFlowGraphExecutionRequest& request) {
                executions.push_back(request.executionId);
                return UIFlowGraphStartResult::running();
            });
        CHECK(runtime.load(asynchronousDocument(policy)));
        CHECK(runtime.start());
        CHECK(runtime.emitSignal("go"));
        CHECK(runtime.emitSignal("next"));
        CHECK(runtime.emitSignal("skip"));
        CHECK(executions.size() == 1u);
        if (executions.empty()) return;
        CHECK(runtime.completeGraphExecution(executions.front()));
        if (policy == UIFlowInterruptPolicy::Coalesce) {
            CHECK(runtime.activeState("main") == "d");
            CHECK(executions.size() == 2u);
            CHECK(runtime.completeGraphExecution(executions.back()));
        } else {
            CHECK(runtime.activeState("main") == "b");
            CHECK(executions.size() == 1u);
        }
        CHECK_FALSE(runtime.hasPendingGraphExecution());
    }
}

TEST_CASE(async_graph_rejection_discards_stale_deferred_region_work)
{
    RecordingScreenHost host;
    UIFlowDocument document =
        asynchronousDocument(UIFlowInterruptPolicy::Queue);
    document.signals.push_back(UIFlowSignalDefinition{"reset", {}});
    document.graphs.push_back(UIFlowGraphDefinition{"graph.reset"});
    document.transitions.push_back(UIFlowTransitionDefinition{
        "reset", "main", "c", "a", "reset", {}, "graph.reset", 0,
        UIFlowInterruptPolicy::Queue});

    std::vector<UIFlowGraphExecutionId> executions;
    UIFlowRuntime runtime(host);
    runtime.setAsyncGraphRequestHandler(
        [&](const UIFlowGraphExecutionRequest& request) {
            if (request.graph.graphId == "graph.next") {
                return UIFlowGraphStartResult::rejected("test rejection");
            }
            executions.push_back(request.executionId);
            return UIFlowGraphStartResult::running();
        });
    CHECK(runtime.load(std::move(document)));
    CHECK(runtime.start());
    CHECK(runtime.emitSignal("go"));
    CHECK(runtime.emitSignal("next"));
    CHECK(runtime.emitSignal("skip"));
    CHECK(executions.size() == 1u);
    if (executions.empty()) return;

    std::string error;
    CHECK_FALSE(runtime.completeGraphExecution(executions.front(), true, {}, &error));
    CHECK(error.find("test rejection") != std::string::npos);
    CHECK(runtime.activeState("main") == "c");
    CHECK_FALSE(runtime.hasPendingGraphExecution());

    CHECK(runtime.emitSignal("reset"));
    CHECK(executions.size() == 2u);
    CHECK(runtime.completeGraphExecution(executions.back()));
    CHECK(runtime.activeState("main") == "a");
    CHECK_FALSE(runtime.hasPendingGraphExecution());
}

TEST_CASE(async_graph_pipeline_waits_for_exit_transition_and_enter_in_order)
{
    RecordingScreenHost host;
    UIFlowDocument document =
        asynchronousDocument(UIFlowInterruptPolicy::Queue);
    document.graphs.push_back(UIFlowGraphDefinition{"graph.exit"});
    document.graphs.push_back(UIFlowGraphDefinition{"graph.enter"});
    document.regions.front().states[0].exitGraph = "graph.exit";
    document.regions.front().states[1].enterGraph = "graph.enter";
    std::vector<std::pair<UIFlowGraphExecutionId, std::string>> executions;
    UIFlowRuntime runtime(host);
    runtime.setAsyncGraphRequestHandler(
        [&](const UIFlowGraphExecutionRequest& request) {
            executions.emplace_back(
                request.executionId, request.graph.graphId);
            return UIFlowGraphStartResult::running();
        });
    CHECK(runtime.load(std::move(document)));
    CHECK(runtime.start());
    CHECK(runtime.emitSignal("go"));
    CHECK(executions.size() == 1u);
    if (executions.size() != 1u) return;
    CHECK(executions[0].second == "graph.exit");
    CHECK(runtime.completeGraphExecution(executions[0].first));
    CHECK(executions.size() == 2u);
    if (executions.size() != 2u) return;
    CHECK(executions[1].second == "graph.go");
    CHECK(runtime.completeGraphExecution(executions[1].first));
    CHECK(executions.size() == 3u);
    if (executions.size() != 3u) return;
    CHECK(executions[2].second == "graph.enter");
    CHECK(runtime.completeGraphExecution(executions[2].first));
    CHECK_FALSE(runtime.hasPendingGraphExecution());
}

TEST_CASE(async_graph_cancel_and_reverse_interrupt_running_execution)
{
    for (const UIFlowInterruptPolicy policy : {
             UIFlowInterruptPolicy::CancelPrevious,
             UIFlowInterruptPolicy::ReversePrevious}) {
        RecordingScreenHost host;
        std::vector<UIFlowGraphExecutionId> executions;
        std::vector<UIFlowGraphInterrupt> interrupts;
        // Callback capture storage must outlive the runtime, whose destructor
        // cancels any still-running host graph execution.
        UIFlowRuntime runtime(host);
        runtime.setAsyncGraphRequestHandler(
            [&executions](const UIFlowGraphExecutionRequest& request) {
                executions.push_back(request.executionId);
                return UIFlowGraphStartResult::running();
            },
            [&interrupts](UIFlowGraphExecutionId, UIFlowGraphInterrupt value) {
                interrupts.push_back(value);
            });
        CHECK(runtime.load(asynchronousDocument(policy)));
        CHECK(runtime.start());
        CHECK(runtime.emitSignal("go"));
        CHECK(runtime.emitSignal("next"));
        CHECK(runtime.activeState("main") == "c");
        CHECK(interrupts.size() == 1u);
        CHECK(interrupts[0] == (policy == UIFlowInterruptPolicy::ReversePrevious
            ? UIFlowGraphInterrupt::Reverse : UIFlowGraphInterrupt::Cancel));
        CHECK(executions.size() == 2u);
    }
}

TEST_CASE(reload_preserves_compatible_manual_context_and_replaces_changed_screen)
{
    RecordingScreenHost host;
    UIFlowRuntime runtime(host);
    UIFlowDocument document = arbitrationDocument();
    CHECK(runtime.load(document));
    CHECK(runtime.start());
    const UIFlowContextHandle pause = runtime.activateContext("Pause");
    CHECK(pause != 0u);
    if (pause == 0u) return;
    const std::uint64_t oldMount = runtime.mountedScreens().front().mountId;
    document.screens[1].layoutAsset = "pause-v2.ui.json";
    host.events.clear();
    CHECK(runtime.reload(std::move(document)));
    CHECK(runtime.mountedScreens().front().screenId == "pause");
    CHECK(runtime.mountedScreens().front().layoutAsset == "pause-v2.ui.json");
    CHECK(runtime.mountedScreens().front().mountId != oldMount);
    CHECK(runtime.deactivateContext(pause));
    CHECK(runtime.mountedScreens().size() == 1u);
    CHECK(runtime.mountedScreens().front().screenId == "menu");
    CHECK_FALSE(runtime.trace().empty());
}

TEST_CASE(async_graph_interrupt_exceptions_are_contained)
{
    RecordingScreenHost host;
    std::vector<UIFlowGraphExecutionId> executions;
    UIFlowRuntime runtime(host);
    CHECK(runtime.load(asynchronousDocument(
        UIFlowInterruptPolicy::CancelPrevious)));
    runtime.setAsyncGraphRequestHandler(
        [&](const UIFlowGraphExecutionRequest& request) {
            executions.push_back(request.executionId);
            return UIFlowGraphStartResult::running();
        },
        [](UIFlowGraphExecutionId, UIFlowGraphInterrupt) {
            throw std::runtime_error("interrupt fault");
        });
    CHECK(runtime.start());
    CHECK(runtime.emitSignal("go"));
    std::string error;
    CHECK_FALSE(runtime.emitSignal("next", {}, &error));
    CHECK(error.find("interrupt fault") != std::string::npos);
    CHECK(runtime.activeState("main") == "b");
    CHECK(runtime.hasPendingGraphExecution("main"));
    CHECK(executions.size() == 1u);
    CHECK(runtime.completeGraphExecution(executions.front()));
}

TEST_CASE(replay_reproduces_signal_sequence_without_recording_it_twice)
{
    RecordingScreenHost sourceHost;
    UIFlowRuntime source(sourceHost);
    CHECK(source.load(asynchronousDocument(UIFlowInterruptPolicy::Queue)));
    CHECK(source.start());
    CHECK(source.emitSignal("go"));
    const auto recording = source.replaySignals();
    CHECK(recording.size() == 1u);
    if (recording.size() != 1u) return;

    RecordingScreenHost targetHost;
    UIFlowRuntime target(targetHost);
    CHECK(target.load(asynchronousDocument(UIFlowInterruptPolicy::Queue)));
    CHECK(target.start());
    CHECK(target.replay(recording));
    CHECK(target.activeState("main") == "b");
    CHECK(target.replaySignals().empty());
}

TEST_CASE(consume_handled_retries_lower_target_and_block_lower_stops_retry)
{
    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(800.0f, 450.0f);
    MouseProbeWidget lower;
    lower.setPosition({600.0f, 340.0f});
    lower.setSize({120.0f, 80.0f});
    manager.root()->addChildExternal(&lower);

    UIManagerFlowScreenHost host(manager, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    UIFlowScreenMountRequest request;
    request.mountId = 40;
    request.screenId = "retry";
    request.layoutAsset = "ui_flow_screen.ui.json";
    request.layerId = "hud";
    request.slotId = "hud.main";
    request.inputPolicy = UIFlowInputPolicy::ConsumeHandled;
    std::string error;
    CHECK(host.mountScreen(request, error));
    CHECK(manager.onMouseButtonDown(640.0f, 380.0f, 0));
    CHECK(lower.pressed);

    lower.pressed = false;
    host.unmountScreen(40);
    request.mountId = 41;
    request.inputPolicy = UIFlowInputPolicy::BlockLower;
    CHECK(host.mountScreen(request, error));
    CHECK(manager.onMouseButtonDown(640.0f, 380.0f, 0));
    CHECK_FALSE(lower.pressed);
}

TEST_CASE(screen_host_hands_off_enter_exit_animation_and_reduced_motion)
{
    ayt::ui::AnimationSettings::get().reset();
    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(800.0f, 450.0f);
    UIManagerFlowScreenHost host(manager, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    UIFlowScreenMountRequest request;
    request.mountId = 50;
    request.screenId = "animated";
    request.layoutAsset = "ui_flow_screen.ui.json";
    request.layerId = "main";
    request.slotId = "main.content";
    request.enterAnimation = "flow.enter";
    request.exitAnimation = "flow.exit";
    std::string error;
    CHECK(host.mountScreen(request, error));
    CHECK(host.screenRoot(50) != nullptr);
    if (host.screenRoot(50) == nullptr) return;
    CHECK(host.screenRoot(50)->getOpacity() < 1.0f);
    host.update(0.2f);
    CHECK(host.screenRoot(50)->getOpacity() == 1.0f);
    host.unmountScreen(50);
    CHECK(host.isScreenRetiring(50));
    host.update(0.2f);
    CHECK_FALSE(host.isScreenRetiring(50));

    ayt::ui::AnimationSettings::get().setReducedMotion(true);
    request.mountId = 51;
    CHECK(host.mountScreen(request, error));
    CHECK(host.screenRoot(51) != nullptr);
    if (host.screenRoot(51) == nullptr) return;
    CHECK(host.screenRoot(51)->getOpacity() == 1.0f);
    host.unmountScreen(51);
    CHECK_FALSE(host.isScreenRetiring(51));
    ayt::ui::AnimationSettings::get().reset();
}

TEST_CASE(flow_asset_validation_builds_deduplicated_deployable_dependencies)
{
    UIFlowDocument document = arbitrationDocument();
    document.screens[0].layoutAsset = "ui_flow_screen.ui.json";
    document.screens[0].enterAnimation = "flow.enter";
    document.screens[1].layoutAsset = "ui_flow_screen.ui.json";
    document.screens[1].exitAnimation = "flow.exit";
    document.signals.push_back({"ui.continue", {}});
    document.screens[0].events.push_back({"continueFlow", "ui.continue"});

    const UIFlowAssetValidationResult result = validateUIFlowAssets(
        document, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    CHECK(result.valid());
    CHECK(result.diagnostics.empty());
    CHECK(result.dependencies.size() == 1u);
    if (result.dependencies.empty()) return;
    CHECK(result.dependencies.front().asset == "ui_flow_screen.ui.json");
    CHECK(result.dependencies.front().screens.size() == 2u);
    CHECK(result.dependencies.front().screens[0] == "menu");
    CHECK(result.dependencies.front().screens[1] == "pause");
}

TEST_CASE(flow_asset_validation_reports_unresolved_layout_event_handler)
{
    UIFlowDocument document = arbitrationDocument();
    document.screens[0].layoutAsset = "ui_flow_screen.ui.json";
    document.signals.push_back({"ui.continue", {}});
    document.screens[0].events.push_back({"missingHandler", "ui.continue"});
    document.screens[1].layoutAsset = "ui_flow_screen.ui.json";

    const UIFlowAssetValidationResult result = validateUIFlowAssets(
        document, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    CHECK_FALSE(result.valid());
    CHECK(result.diagnostics.size() == 1u);
    CHECK(result.diagnostics.front().path
          == "$.screens[0].events[0].handler");
}

TEST_CASE(flow_asset_validation_indexes_events_in_inactive_structured_content)
{
    UIFlowDocument document = arbitrationDocument();
    document.screens[0].layoutAsset = "ui_flow_tab_screen.ui.json";
    document.screens[1].layoutAsset = "ui_flow_tab_screen.ui.json";
    document.signals.push_back({"ui.continue", {}});
    document.screens[0].events.push_back(
        {"continueFromInactiveTab", "ui.continue"});

    const UIFlowAssetValidationResult result = validateUIFlowAssets(
        document, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    CHECK(result.valid());
    CHECK(result.diagnostics.empty());
}

TEST_CASE(flow_asset_validation_reports_missing_clips_files_and_root_escape)
{
    UIFlowDocument document = arbitrationDocument();
    document.screens[0].layoutAsset = "ui_flow_screen.ui.json";
    document.screens[0].enterAnimation = "missing.clip";
    document.screens[1].layoutAsset = "missing.ui.json";

    UIFlowAssetValidationResult result = validateUIFlowAssets(
        document, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    CHECK_FALSE(result.valid());
    CHECK(result.diagnostics.size() == 2u);
    CHECK(result.diagnostics[0].path == "$.screens[0].enterAnimation");
    CHECK(result.diagnostics[1].path == "$.screens[1].layoutAsset");

    document.screens[0].enterAnimation.clear();
    document.screens[1].layoutAsset = "../outside.ui.json";
    result = validateUIFlowAssets(document, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    CHECK_FALSE(result.valid());
    CHECK(result.diagnostics.size() == 1u);
    CHECK(result.diagnostics.front().path == "$.screens[1].layoutAsset");
    CHECK(result.diagnostics.front().message.find("escapes")
          != std::string::npos);

    document.screens[0].layoutAsset = "ui_flow_broken_track.ui.json";
    document.screens[0].enterAnimation = "broken.enter";
    document.screens[1].layoutAsset = "ui_flow_screen.ui.json";
    result = validateUIFlowAssets(document, AY_APPLICATION_UI_TEST_ASSET_ROOT);
    CHECK_FALSE(result.valid());
    CHECK(result.diagnostics.size() == 1u);
    CHECK(result.diagnostics.front().path
          == "$.screens[0].enterAnimation");
    CHECK(result.diagnostics.front().message.find("unresolved")
          != std::string::npos);
}

TEST_SUITE_END

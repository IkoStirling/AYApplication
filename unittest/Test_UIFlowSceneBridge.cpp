#include <AYApplication/UIFlowSceneBridge.h>
#include <AYApplication/UIFlowSceneComponents.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/SceneSerializer.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
#include <AYScene.h>
#include <AYTest.h>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace ayt::app;
using namespace ayt::ui;

class SceneBridgeScreenHost final : public IUIFlowScreenHost
{
public:
    bool mountScreen(
        const UIFlowScreenMountRequest& request,
        std::string& error) override
    {
        if (request.screenId == rejectedScreen) {
            error = "deliberate Scene bridge mount rejection";
            return false;
        }
        active[request.mountId] = request.screenId;
        return true;
    }

    void unmountScreen(std::uint64_t mountId) noexcept override
    {
        active.erase(mountId);
    }

    void setScreenOrder(
        std::uint64_t,
        int,
        std::uint32_t) noexcept override
    {
    }

    std::map<std::uint64_t, std::string> active;
    std::string rejectedScreen;
};

UIFlowDocument sceneBridgeDocument()
{
    UIFlowDocument document;
    document.id = "scene-bridge";
    document.defaultEntry = "boot";
    document.layers = {
        UIFlowLayerDefinition{"application", 0},
        UIFlowLayerDefinition{"hud", 100},
        UIFlowLayerDefinition{"area", 200},
        UIFlowLayerDefinition{"story", 300},
        UIFlowLayerDefinition{"debug", 400},
    };
    document.slots = {
        UIFlowSlotDefinition{"application.main", "application", 1, true},
        UIFlowSlotDefinition{"hud.main", "hud", 1, true},
        UIFlowSlotDefinition{"area.main", "area", 1, true},
        UIFlowSlotDefinition{"story.main", "story", 1, true},
        UIFlowSlotDefinition{"debug.main", "debug", 1, true},
    };
    document.screens = {
        UIFlowScreenDefinition{
            "shell", "shell.ui.json", "application", "application.main",
            UIFlowScope::Application},
        UIFlowScreenDefinition{
            "hud", "hud.ui.json", "hud", "hud.main",
            UIFlowScope::World},
        UIFlowScreenDefinition{
            "area-prompt", "area.ui.json", "area", "area.main",
            UIFlowScope::World},
        UIFlowScreenDefinition{
            "story-panel", "story.ui.json", "story", "story.main",
            UIFlowScope::World},
        UIFlowScreenDefinition{
            "owner-prompt", "owner.ui.json", "story", "story.main",
            UIFlowScope::Owner},
        UIFlowScreenDefinition{
            "debug-overlay", "debug.ui.json", "debug", "debug.main",
            UIFlowScope::Application},
    };
    document.contexts = {
        UIFlowContextDefinition{
            "Boot", 0,
            {{"application.main", UIFlowSlotOperation::Present, "shell"}}},
        UIFlowContextDefinition{
            "Town", 0,
            {{"hud.main", UIFlowSlotOperation::Present, "hud"}}},
        UIFlowContextDefinition{
            "InsideArea", 0,
            {{"area.main", UIFlowSlotOperation::Present, "area-prompt"}}},
        UIFlowContextDefinition{
            "Story", 0,
            {{"story.main", UIFlowSlotOperation::Present, "story-panel"}}},
        UIFlowContextDefinition{
            "OwnerPrompt", 0,
            {{"story.main", UIFlowSlotOperation::Present, "owner-prompt"}}},
        UIFlowContextDefinition{
            "PersistentDebug", 0,
            {{"debug.main", UIFlowSlotOperation::Present, "debug-overlay"}}},
    };
    document.entries.push_back(UIFlowEntryDefinition{"boot", {"Boot"}, {}});
    document.signals = {
        UIFlowSignalDefinition{"scene.currentChanged", {}},
        UIFlowSignalDefinition{"scene.beginPlay", {}},
        UIFlowSignalDefinition{"scene.endPlay", {}},
        UIFlowSignalDefinition{"zone.enter", {}},
        UIFlowSignalDefinition{"zone.exit", {}},
    };
    document.regions.push_back(UIFlowRegionDefinition{
        "area", "outside",
        {
            UIFlowStateDefinition{"outside"},
            UIFlowStateDefinition{
                "inside", {}, {}, {"InsideArea"}},
        }});
    document.transitions = {
        UIFlowTransitionDefinition{
            "area.enter", "area", "outside", "inside", "zone.enter"},
        UIFlowTransitionDefinition{
            "area.exit", "area", "inside", "outside", "zone.exit"},
    };
    return document;
}

bool containsScreen(
    const UIFlowRuntime& runtime,
    std::string_view screenId)
{
    for (const UIFlowMountedScreen& screen : runtime.mountedScreens()) {
        if (screen.screenId == screenId) return true;
    }
    return false;
}

bool ensureSceneBridgeComponentsRegistered()
{
    ayt::entity::ComponentRegistry& registry =
        ayt::entity::ComponentRegistry::instance();
    if (registry.find<ayt::entity::Transform>() == nullptr) {
        if (!ayt::entity::registerEntityCoreComponents(registry)) return false;
    }
    if (registry.find<SceneSignalVolumeComponent>() == nullptr
        || registry.find<SceneSignalParticipantComponent>() == nullptr) {
        if (!registerUIFlowSceneComponents(registry)) return false;
    }
    return true;
}

} // namespace

TEST_SUITE(UIFlowSceneBridgeTests)

TEST_CASE(scene_lifecycle_drives_world_scope_context_and_declared_signals)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());
    CHECK(containsScreen(runtime, "shell"));
    CHECK_FALSE(containsScreen(runtime, "hud"));

    ayt::event::EventBus eventBus;
    ayt::scene::Scene town(ayt::scene::SceneMode::Play, "town");
    UIFlowSceneBridgeConfig config;
    config.worldContexts.push_back({"town", "Town"});
    config.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };

    std::vector<std::string> observed;
    runtime.subscribeSignal("*",
        [&observed](std::string_view signal, const UIFlowPayload&) {
            observed.emplace_back(signal);
        });
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start(&town));
    CHECK(bridge.currentWorldKey() == "town");
    CHECK(runtime.scopeKey(UIFlowScope::World) == "town");
    CHECK(containsScreen(runtime, "hud"));
    CHECK(observed.size() == 1u);
    CHECK(observed[0] == "scene.currentChanged");

    ayt::event::SceneBeginPlayEvent begin;
    begin.play = &town;
    eventBus.emit(begin);
    CHECK(observed.size() == 2u);
    CHECK(observed[1] == "scene.beginPlay");

    ayt::event::SceneCurrentChangedEvent current;
    current.current = nullptr;
    eventBus.emit(current);
    CHECK(bridge.currentScene() == nullptr);
    CHECK(runtime.scopeKey(UIFlowScope::World).empty());
    CHECK_FALSE(containsScreen(runtime, "hud"));
    CHECK(containsScreen(runtime, "shell"));
    CHECK(observed.size() == 3u);
    CHECK(observed[2] == "scene.currentChanged");

    ayt::event::SceneEndPlayEvent end;
    end.edit = nullptr;
    eventBus.emit(end);
    CHECK(observed.size() == 4u);
    CHECK(observed[3] == "scene.endPlay");
    CHECK(bridge.stats().sceneChanges == 2u);
    CHECK(bridge.stats().emittedSignals == 4u);
    CHECK(bridge.stats().rejectedSignals == 0u);
}

TEST_CASE(scene_signal_volume_drives_flow_transition_and_once_policy)
{
    CHECK(ensureSceneBridgeComponentsRegistered());

    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    ayt::scene::Scene arena(ayt::scene::SceneMode::Play, "arena");
    UIFlowSceneBridgeConfig config;
    config.currentChangedSignal.clear();
    config.beginPlaySignal.clear();
    config.endPlaySignal.clear();
    config.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start(&arena));

    ayt::entity::Entity* volumeEntity = arena.world().createEntity();
    ayt::entity::Entity* participantEntity = arena.world().createEntity();
    CHECK(volumeEntity != nullptr);
    CHECK(participantEntity != nullptr);
    if (volumeEntity == nullptr || participantEntity == nullptr) return;
    volumeEntity->setName("gate");
    participantEntity->setName("player");

    auto* volumeTransform =
        volumeEntity->addComponent<ayt::entity::Transform>();
    auto* volume = volumeEntity->addComponent<SceneSignalVolumeComponent>();
    auto* participantTransform =
        participantEntity->addComponent<ayt::entity::Transform>();
    auto* participant =
        participantEntity->addComponent<SceneSignalParticipantComponent>();
    CHECK(volumeTransform != nullptr);
    CHECK(volume != nullptr);
    CHECK(participantTransform != nullptr);
    CHECK(participant != nullptr);
    if (volumeTransform == nullptr || volume == nullptr
        || participantTransform == nullptr || participant == nullptr) {
        return;
    }

    volume->sourceId = "town-gate";
    volume->enterSignal = "zone.enter";
    volume->exitSignal = "zone.exit";
    volume->participantTag = "player";
    volume->halfExtents = ayt::math::FVector3(2.0f, 2.0f, 2.0f);
    volume->emitOncePerWorld = true;
    participant->tag = "player";
    participantTransform->setPosition(5.0f, 0.0f, 0.0f);

    std::vector<std::string> observed;
    UIFlowPayload firstEnterPayload;
    runtime.subscribeSignal("*",
        [&observed, &firstEnterPayload](
            std::string_view signal,
            const UIFlowPayload& payload) {
            observed.emplace_back(signal);
            if (signal == "zone.enter" && firstEnterPayload.empty()) {
                firstEnterPayload = payload;
            }
        });

    CHECK(bridge.update(&arena));
    CHECK(observed.empty());
    participantTransform->setPosition(1.0f, 0.0f, 0.0f);
    CHECK(bridge.update(&arena));
    CHECK(runtime.activeState("area") == "inside");
    CHECK(containsScreen(runtime, "area-prompt"));
    CHECK(observed.size() == 1u);
    CHECK(observed[0] == "zone.enter");
    CHECK(std::get<std::string>(firstEnterPayload.at("sourceId").data)
        == "town-gate");
    CHECK(std::get<std::string>(firstEnterPayload.at("participantTag").data)
        == "player");
    CHECK(std::get<std::string>(firstEnterPayload.at("world").data)
        == "arena");

    CHECK(bridge.update(&arena));
    CHECK(observed.size() == 1u);
    participantTransform->setPosition(5.0f, 0.0f, 0.0f);
    CHECK(bridge.update(&arena));
    CHECK(runtime.activeState("area") == "outside");
    CHECK_FALSE(containsScreen(runtime, "area-prompt"));
    CHECK(observed.size() == 2u);
    CHECK(observed[1] == "zone.exit");

    participantTransform->setPosition(0.0f, 0.0f, 0.0f);
    CHECK(bridge.update(&arena));
    participantTransform->setPosition(5.0f, 0.0f, 0.0f);
    CHECK(bridge.update(&arena));
    CHECK(observed.size() == 2u);
    CHECK(bridge.stats().volumeEnters == 2u);
    CHECK(bridge.stats().volumeExits == 2u);
    CHECK(bridge.stats().emittedSignals == 2u);
    CHECK(bridge.stats().rejectedSignals == 0u);
}

TEST_CASE(explicit_scene_signal_api_validates_against_flow_contract)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    UIFlowSceneBridgeConfig config;
    config.currentChangedSignal.clear();
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start());

    UIFlowSceneSignalEvent event;
    event.sourceId = "lever-a";
    event.participantEntity = "entity:42";
    event.payload["custom"] = std::int64_t(7);
    CHECK(bridge.emitSceneSignal("zone.enter", event));
    CHECK(runtime.activeState("area") == "inside");

    std::string error;
    CHECK_FALSE(bridge.emitSceneSignal("missing.signal", {}, &error));
    CHECK(error.find("Unknown UI Flow Signal") != std::string::npos);
    CHECK(bridge.stats().emittedSignals == 1u);
    CHECK(bridge.stats().rejectedSignals == 1u);
}

TEST_CASE(scene_signal_requests_can_be_published_through_the_event_bus)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    UIFlowSceneBridgeConfig config;
    config.currentChangedSignal.clear();
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start());

    UIFlowSceneSignalRequestEvent request;
    request.signalId = "zone.enter";
    request.event.sourceId = "physics-trigger";
    request.event.payload["reason"] = std::string("overlap");
    eventBus.emit(request);
    CHECK(runtime.activeState("area") == "inside");
    CHECK(bridge.lastError().empty());

    request.signalId = "missing.signal";
    eventBus.emit(request);
    CHECK(bridge.lastError().find("Unknown UI Flow Signal")
        != std::string_view::npos);
    CHECK(bridge.stats().emittedSignals == 1u);
    CHECK(bridge.stats().rejectedSignals == 1u);
}

TEST_CASE(scene_presentations_support_source_release_and_scene_lifetimes)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    ayt::scene::Scene town(ayt::scene::SceneMode::Play, "town");
    ayt::scene::Scene arena(ayt::scene::SceneMode::Play, "arena");
    UIFlowSceneBridgeConfig config;
    config.currentChangedSignal.clear();
    config.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start(&town));

    const UIFlowScenePresentationHandle story = bridge.pushPresentation({
        "Story", "story-zone", UIFlowScope::World, {}});
    const UIFlowScenePresentationHandle owner = bridge.pushPresentation({
        "OwnerPrompt", "cinematic-42", UIFlowScope::Owner, {}});
    const UIFlowScenePresentationHandle debug = bridge.pushPresentation({
        "PersistentDebug", "debug-service",
        UIFlowScope::Application, {}});
    CHECK(story != 0);
    CHECK(owner != 0);
    CHECK(debug != 0);
    CHECK(bridge.activePresentationCount() == 3u);
    CHECK(containsScreen(runtime, "owner-prompt"));
    CHECK(containsScreen(runtime, "debug-overlay"));

    CHECK(bridge.releasePresentations("cinematic-42") == 1u);
    CHECK(bridge.activePresentationCount() == 2u);
    CHECK_FALSE(containsScreen(runtime, "owner-prompt"));
    CHECK(bridge.update(&arena));
    CHECK(bridge.activePresentationCount() == 1u);
    CHECK_FALSE(containsScreen(runtime, "story-panel"));
    CHECK(containsScreen(runtime, "debug-overlay"));
    CHECK(bridge.popPresentation(debug));
    CHECK(bridge.activePresentationCount() == 0u);
    CHECK_FALSE(containsScreen(runtime, "debug-overlay"));
    CHECK(bridge.stats().presentationPushes == 3u);
    CHECK(bridge.stats().presentationPops == 3u);
}

TEST_CASE(scene_presentations_retire_when_scene_instance_keeps_world_key)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    ayt::scene::Scene first(ayt::scene::SceneMode::Play, "shared");
    ayt::scene::Scene second(ayt::scene::SceneMode::Play, "shared");
    UIFlowSceneBridgeConfig config;
    config.currentChangedSignal.clear();
    config.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start(&first));
    CHECK(bridge.pushPresentation({
        "Story", "first-instance", UIFlowScope::World, {}}) != 0);
    CHECK(containsScreen(runtime, "story-panel"));

    CHECK(bridge.update(&second));
    CHECK(bridge.currentScene() == &second);
    CHECK(bridge.activePresentationCount() == 0u);
    CHECK_FALSE(containsScreen(runtime, "story-panel"));
}

TEST_CASE(world_context_bindings_support_defaults_and_multiple_contexts)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    ayt::scene::Scene town(ayt::scene::SceneMode::Play, "town");
    UIFlowSceneBridgeConfig config;
    config.currentChangedSignal.clear();
    config.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    config.worldContexts = {
        {"*", "Story"},
        {"town", "Town"},
        {"town", "InsideArea"},
    };
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start(&town));
    CHECK(containsScreen(runtime, "story-panel"));
    CHECK(containsScreen(runtime, "hud"));
    CHECK(containsScreen(runtime, "area-prompt"));
}

TEST_CASE(scene_scope_sync_retries_after_a_transactional_mount_failure)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    UIFlowSceneBridgeConfig config;
    config.worldContexts.push_back({"town", "Town"});
    config.worldKeyResolver = [](const ayt::scene::Scene& scene) {
        return scene.name();
    };
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    CHECK(bridge.start());

    ayt::scene::Scene town(ayt::scene::SceneMode::Play, "town");
    screenHost.rejectedScreen = "hud";
    ayt::event::SceneCurrentChangedEvent current;
    current.current = &town;
    eventBus.emit(current);
    CHECK_FALSE(bridge.lastError().empty());
    CHECK(runtime.scopeKey(UIFlowScope::World).empty());
    CHECK_FALSE(containsScreen(runtime, "hud"));

    screenHost.rejectedScreen.clear();
    std::string error;
    CHECK(bridge.update(&town, &error));
    CHECK(error.empty());
    CHECK(bridge.currentWorldKey() == "town");
    CHECK(runtime.scopeKey(UIFlowScope::World) == "town");
    CHECK(containsScreen(runtime, "hud"));
}

TEST_CASE(scene_bridge_rejects_invalid_world_context_configuration)
{
    SceneBridgeScreenHost screenHost;
    UIFlowRuntime runtime(screenHost);
    CHECK(runtime.load(sceneBridgeDocument()));
    CHECK(runtime.start());

    ayt::event::EventBus eventBus;
    UIFlowSceneBridgeConfig config;
    config.worldContexts.push_back({"town", "MissingContext"});
    UIFlowSceneBridge bridge(runtime, eventBus, std::move(config));
    std::string error;
    CHECK_FALSE(bridge.start(nullptr, &error));
    CHECK(error.find("unknown Context") != std::string::npos);
    CHECK_FALSE(bridge.isStarted());
}

TEST_CASE(scene_signal_components_are_editor_addable_and_serializable)
{
    CHECK(ensureSceneBridgeComponentsRegistered());
    const ayt::entity::ComponentRegistry& registry =
        ayt::entity::ComponentRegistry::instance();
    const ayt::entity::ComponentDescriptor* volume =
        registry.find<SceneSignalVolumeComponent>();
    const ayt::entity::ComponentDescriptor* participant =
        registry.find<SceneSignalParticipantComponent>();
    CHECK(volume != nullptr);
    CHECK(participant != nullptr);
    if (volume == nullptr || participant == nullptr) return;
    CHECK(volume->editorAddable);
    CHECK(volume->sceneSerializable);
    CHECK(volume->displayName == "Scene Signal Volume");
    CHECK(participant->editorAddable);
    CHECK(participant->sceneSerializable);
    CHECK(participant->displayName == "Scene Signal Participant");
}

TEST_CASE(scene_signal_components_roundtrip_through_scene_serializer)
{
    CHECK(ensureSceneBridgeComponentsRegistered());

    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "authoring");
    ayt::entity::Entity* volumeEntity = scene.world().createEntity();
    ayt::entity::Entity* participantEntity = scene.world().createEntity();
    CHECK(volumeEntity != nullptr);
    CHECK(participantEntity != nullptr);
    if (volumeEntity == nullptr || participantEntity == nullptr) return;
    volumeEntity->setName("ui-region");
    participantEntity->setName("ui-participant");

    auto* volume = volumeEntity->addComponent<SceneSignalVolumeComponent>();
    auto* participant =
        participantEntity->addComponent<SceneSignalParticipantComponent>();
    CHECK(volume != nullptr);
    CHECK(participant != nullptr);
    if (volume == nullptr || participant == nullptr) return;

    volume->sourceId = "story-gate";
    volume->enterSignal = "zone.enter";
    volume->exitSignal = "zone.exit";
    volume->participantTag = "camera";
    volume->halfExtents = ayt::math::FVector3(4.0f, 2.0f, 6.0f);
    volume->offset = ayt::math::FVector3(1.0f, -2.0f, 3.0f);
    volume->enabled = false;
    volume->emitOncePerWorld = true;
    participant->tag = "camera";
    participant->enabled = false;

    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / "ay_ui_flow_scene_components.ayscene";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    CHECK(ayt::entity::saveScene(scene.world(), path.string()));

    scene.world().destroyEntity(volumeEntity);
    scene.world().destroyEntity(participantEntity);
    ayt::serializer::SerializeError error;
    CHECK(ayt::entity::loadScene(scene.world(), path.string(), &error));
    CHECK(error.ok());

    ayt::entity::Entity* loadedVolumeEntity =
        scene.world().findEntity("ui-region");
    ayt::entity::Entity* loadedParticipantEntity =
        scene.world().findEntity("ui-participant");
    CHECK(loadedVolumeEntity != nullptr);
    CHECK(loadedParticipantEntity != nullptr);
    if (loadedVolumeEntity != nullptr) {
        const auto* loaded = loadedVolumeEntity
            ->getComponent<SceneSignalVolumeComponent>();
        CHECK(loaded != nullptr);
        if (loaded != nullptr) {
            CHECK(loaded->sourceId == "story-gate");
            CHECK(loaded->enterSignal == "zone.enter");
            CHECK(loaded->exitSignal == "zone.exit");
            CHECK(loaded->participantTag == "camera");
            CHECK_FLOAT_EQ(loaded->halfExtents.x, 4.0f, 0.0001f);
            CHECK_FLOAT_EQ(loaded->halfExtents.y, 2.0f, 0.0001f);
            CHECK_FLOAT_EQ(loaded->halfExtents.z, 6.0f, 0.0001f);
            CHECK_FLOAT_EQ(loaded->offset.x, 1.0f, 0.0001f);
            CHECK_FLOAT_EQ(loaded->offset.y, -2.0f, 0.0001f);
            CHECK_FLOAT_EQ(loaded->offset.z, 3.0f, 0.0001f);
            CHECK_FALSE(loaded->enabled);
            CHECK(loaded->emitOncePerWorld);
        }
    }
    if (loadedParticipantEntity != nullptr) {
        const auto* loaded = loadedParticipantEntity
            ->getComponent<SceneSignalParticipantComponent>();
        CHECK(loaded != nullptr);
        if (loaded != nullptr) {
            CHECK(loaded->tag == "camera");
            CHECK_FALSE(loaded->enabled);
        }
    }

    std::filesystem::remove(path, removeError);
}

TEST_SUITE_END

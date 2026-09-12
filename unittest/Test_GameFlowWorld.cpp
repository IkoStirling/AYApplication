#include <AYApplicationGameFlowWorld.h>

#include <AYApplication.h>
#include <AYEventSystem/EventBus.h>
#include <AYScene/SceneManager.h>
#include <AYTest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace ayt::app;

class LoaderBackedWorldRouter final : public IGameWorldRouter
{
public:
    LoaderBackedWorldRouter(
        IRuntimeSceneLoader& loader,
        std::vector<GameWorld> worlds,
        std::string currentWorld)
        : _loader(loader),
          _worlds(std::move(worlds)),
          _currentWorld(std::move(currentWorld))
    {
    }

    bool requestWorld(std::string_view worldId) override
    {
        if (!_pendingWorld.empty()) {
            _lastError = "A World transition is already pending";
            return false;
        }
        if (worldId == _currentWorld) {
            _lastError.clear();
            return true;
        }
        const GameWorld* world = findWorld(worldId);
        if (world == nullptr) {
            _lastError = "Unknown World id: " + std::string(worldId);
            return false;
        }
        RuntimeSceneLoadRequest request;
        request.requestId = _nextRequestId++;
        request.scenePath = world->scenePath;
        request.sceneName = world->sceneName.empty()
            ? world->id : world->sceneName;
        request.prepareActivation = world->prepareActivation;
        if (!_loader.requestLoad(std::move(request))) {
            _lastError = "RuntimeSceneLoader rejected the World transition";
            return false;
        }
        _pendingRequestId = _nextRequestId - 1;
        _pendingWorld = world->id;
        _lastError.clear();
        return true;
    }

    void update()
    {
        if (_pendingWorld.empty()) return;
        const RuntimeSceneLoadStatus status = _loader.getLoadStatus();
        if (status.requestId != _pendingRequestId) return;
        if (status.state == RuntimeSceneLoadState::Ready) {
            _currentWorld = std::move(_pendingWorld);
            _pendingWorld.clear();
            _pendingRequestId = 0;
            _lastError.clear();
        } else if (status.state == RuntimeSceneLoadState::Failed
                   || status.state == RuntimeSceneLoadState::Cancelled) {
            _lastError = status.message;
            _pendingWorld.clear();
            _pendingRequestId = 0;
        }
    }

    std::string_view currentWorldId() const noexcept override
    {
        return _currentWorld;
    }

    std::string_view pendingWorldId() const noexcept override
    {
        return _pendingWorld;
    }

    std::string_view lastError() const noexcept override
    {
        return _lastError;
    }

    const GameWorld* findWorld(std::string_view worldId) const noexcept override
    {
        for (const auto& world : _worlds) {
            if (world.id == worldId) return &world;
        }
        return nullptr;
    }

private:
    IRuntimeSceneLoader& _loader;
    std::vector<GameWorld> _worlds;
    std::string _currentWorld;
    std::string _pendingWorld;
    std::string _lastError;
    std::uint64_t _nextRequestId = 1;
    std::uint64_t _pendingRequestId = 0;
};

std::filesystem::path makeSceneFile(std::string_view stem)
{
    const auto path = std::filesystem::temp_directory_path()
        / (std::string(stem) + ".ayscene");
    std::ofstream output(path, std::ios::trunc);
    output << "{}";
    return path;
}

GameFlowDocument makeWorldFlow(std::string worldId)
{
    GameFlowDocument document;
    document.id = "world-flow";
    document.initialState = "menu";
    document.intents = {{"start", {}}};
    document.states = {{"menu"}, {"playing"}, {"load_error"}};

    GameFlowTransitionDefinition transition;
    transition.id = "start-game";
    transition.fromState = "menu";
    transition.triggerIntent = "start";
    transition.toState = "playing";
    transition.actions = {{
        std::string(kGameFlowActionWorldReplace),
        {{"worldId", GameFlowValue(std::move(worldId))}}}};
    transition.onFailureState = "load_error";
    transition.onCancelState = "menu";
    transition.timeoutSeconds = 5.0;
    document.transitions.push_back(std::move(transition));
    return document;
}

struct WorldFlowHarness
{
    explicit WorldFlowHarness(std::vector<GameWorld> worlds)
        : scenes(defaultEngineHost().scenes()),
          loader(createRuntimeSceneLoader(*scenes, {}, &bus)),
          router(*loader, std::move(worlds), "menu"),
          adapter(registry, coordinator, router, *loader, bus)
    {
        scenes->setCurrent(nullptr);
        CHECK(loader->initialize());
        CHECK(adapter.install(&error));
    }

    ~WorldFlowHarness()
    {
        adapter.uninstall();
        loader->shutdown();
    }

    bool prepare(std::string worldId = "game")
    {
        std::vector<GameFlowDiagnostic> diagnostics;
        if (!buildGameFlowPlan(
                makeWorldFlow(std::move(worldId)),
                registry, plan, &diagnostics)) return false;
        if (!diagnostics.empty()) return false;
        return coordinator.setPlan(&plan, &registry, &error);
    }

    void start()
    {
        CHECK(coordinator.request("start"));
        coordinator.update();
    }

    ayt::event::EventBus bus;
    ayt::scene::SceneManager* scenes = nullptr;
    std::unique_ptr<IRuntimeSceneLoader> loader;
    LoaderBackedWorldRouter router;
    GameFlowActionRegistry registry;
    GameFlowPlan plan;
    GameFlowCoordinator coordinator;
    GameFlowWorldActionAdapter adapter;
    std::string error;
};

} // namespace

TEST_SUITE(GameFlowWorldActionTests)

TEST_CASE(real_scene_success_completes_the_pending_transition_once)
{
    const auto scenePath = makeSceneFile("ay_gameflow_world_ok");
    {
        WorldFlowHarness harness({
            {"menu", scenePath.string(), "menu"},
            {"game", scenePath.string(), "game"},
        });
        CHECK(harness.prepare());
        auto* original = harness.loader->currentScene();

        harness.start();
        CHECK(harness.coordinator.busy());
        CHECK(harness.adapter.pendingSceneRequestId() == 1u);
        CHECK(harness.loader->currentScene() == original);

        harness.loader->update(0.0f);
        CHECK(harness.loader->currentScene() != original);
        harness.router.update();
        harness.bus.pump();
        CHECK(harness.coordinator.currentState() == "playing");
        CHECK(!harness.coordinator.busy());
        CHECK(harness.router.currentWorldId() == "game");
        CHECK(harness.adapter.pendingSceneRequestId() == 0u);
    }
    std::filesystem::remove(scenePath);
}

TEST_CASE(failed_scene_load_preserves_the_old_world_and_uses_failure_route)
{
    const std::string missing =
        "Z:/__ay_missing__/gameflow-world.ayscene";
    WorldFlowHarness harness({
        {"menu", missing, "menu"},
        {"game", missing, "game"},
    });
    CHECK(harness.prepare());
    auto* original = harness.loader->currentScene();

    harness.start();
    harness.loader->update(0.0f);
    CHECK(harness.loader->currentScene() == original);
    harness.router.update();
    harness.bus.pump();

    CHECK(harness.coordinator.currentState() == "load_error");
    CHECK(!harness.coordinator.busy());
    CHECK(harness.router.currentWorldId() == "menu");
    CHECK(harness.router.pendingWorldId().empty());
}

TEST_CASE(cancelled_replacement_ignores_a_late_completion)
{
    const auto scenePath = makeSceneFile("ay_gameflow_world_cancel");
    {
        WorldFlowHarness harness({
            {"menu", scenePath.string(), "menu"},
            {"game", scenePath.string(), "game"},
        });
        CHECK(harness.prepare());
        harness.start();
        const auto requestId = harness.adapter.pendingSceneRequestId();
        CHECK(requestId != 0u);

        CHECK(harness.coordinator.cancelActive("user cancelled"));
        CHECK(harness.loader->getLoadStatus().state
              == RuntimeSceneLoadState::Cancelled);
        CHECK(harness.coordinator.currentState() == "menu");
        CHECK(harness.adapter.pendingSceneRequestId() == 0u);

        harness.bus.emit(RuntimeSceneLoadFinishedEvent{requestId, true});
        CHECK(harness.coordinator.currentState() == "menu");
        CHECK(!harness.coordinator.busy());
    }
    std::filesystem::remove(scenePath);
}

TEST_CASE(mismatched_completion_and_concurrent_world_request_are_rejected)
{
    const auto scenePath = makeSceneFile("ay_gameflow_world_busy");
    {
        WorldFlowHarness harness({
            {"menu", scenePath.string(), "menu"},
            {"game", scenePath.string(), "game"},
        });
        CHECK(harness.prepare());
        CHECK(harness.router.requestWorld("game"));

        harness.start();
        CHECK(harness.coordinator.currentState() == "load_error");
        CHECK(!harness.coordinator.busy());

        harness.loader->cancelLoad(1);
        harness.router.update();
        CHECK(harness.coordinator.setPlan(
            &harness.plan, &harness.registry, &harness.error));
        harness.start();
        const auto requestId = harness.adapter.pendingSceneRequestId();
        CHECK(requestId == 2u);
        harness.bus.emit(RuntimeSceneLoadFinishedEvent{
            requestId + 10u, true});
        CHECK(harness.coordinator.busy());
        CHECK(harness.coordinator.currentState() == "menu");

        harness.loader->update(0.0f);
        harness.router.update();
        harness.bus.pump();
        CHECK(harness.coordinator.currentState() == "playing");
    }
    std::filesystem::remove(scenePath);
}

TEST_CASE(current_world_completes_synchronously_without_loading)
{
    const auto scenePath = makeSceneFile("ay_gameflow_world_same");
    {
        WorldFlowHarness harness({
            {"menu", scenePath.string(), "menu"},
        });
        CHECK(harness.prepare("menu"));
        harness.start();
        CHECK(harness.coordinator.currentState() == "playing");
        CHECK(!harness.coordinator.busy());
        CHECK(harness.adapter.pendingSceneRequestId() == 0u);
        CHECK(harness.loader->getLoadStatus().state
              != RuntimeSceneLoadState::Queued);
    }
    std::filesystem::remove(scenePath);
}

TEST_CASE(uninstall_restores_the_handler_owned_by_the_composition_root)
{
    const auto scenePath = makeSceneFile("ay_gameflow_world_restore");
    {
        WorldFlowHarness harness({
            {"menu", scenePath.string(), "menu"},
        });
        harness.adapter.uninstall();

        int invocationCount = 0;
        CHECK(harness.registry.setActionHandler(
            kGameFlowActionWorldReplace,
            [&invocationCount](const GameFlowActionInvocation&) {
                ++invocationCount;
                return GameFlowActionResult::succeeded();
            }, &harness.error));
        CHECK(harness.adapter.install(&harness.error));
        harness.adapter.uninstall();

        const auto* restored = harness.registry.findActionHandler(
            kGameFlowActionWorldReplace);
        CHECK_NOT_NULL(restored);
        if (restored != nullptr) {
            (void)(*restored)(GameFlowActionInvocation{});
        }
        CHECK(invocationCount == 1);
    }
    std::filesystem::remove(scenePath);
}

TEST_SUITE_END

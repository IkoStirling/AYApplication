#include <AYApplication.h>

#include <AYEventSystem/EventBus.h>
#include <AYScene.h>
#include <AYScene/SceneManager.h>
#include <AYTest.h>

#include <filesystem>
#include <fstream>
#include <utility>

using namespace ayt::app;

namespace
{

std::filesystem::path makeEmptySceneFile(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream output(path, std::ios::trunc);
    output << "{}";
    return path;
}

} // namespace

TEST_SUITE(RuntimeSceneLoaderTest)

TEST_CASE(switches_scene_at_update_boundary_and_publishes_completion)
{
    ayt::event::EventBus bus;
    auto* scenes = defaultEngineHost().scenes();
    CHECK(scenes != nullptr);
    scenes->setCurrent(nullptr);

    int activationCount = 0;
    RuntimeSceneLoaderConfig config;
    config.initialSceneName = "initial";
    config.onSceneActivated = [&](ayt::scene::Scene&) { ++activationCount; };
    auto loader = createRuntimeSceneLoader(*scenes, config, &bus);
    CHECK(loader->initialize());
    CHECK(loader->currentScene() != nullptr);
    CHECK(loader->currentScene()->name() == "initial");
    CHECK(activationCount == 1);

    bool finished = false;
    uint64_t finishedId = 0;
    const auto connection = bus.subscribe<RuntimeSceneLoadFinishedEvent>(
        [&](const RuntimeSceneLoadFinishedEvent& event) {
            finished = event.success;
            finishedId = event.requestId;
        });

    const auto path = makeEmptySceneFile("ay_runtime_scene_loader_ok.ayscene");
    auto* original = loader->currentScene();
    CHECK(loader->requestLoad({7, path.string(), "online-world"}));
    CHECK(loader->currentScene() == original);

    loader->update(0.0f);
    CHECK(loader->currentScene() != original);
    CHECK(loader->currentScene() == scenes->current());
    CHECK(loader->currentScene()->name() == "online-world");
    CHECK(activationCount == 2);
    CHECK_FALSE(finished);
    bus.pump();
    CHECK(finished);
    CHECK(finishedId == 7);

    bus.unsubscribe(connection);
    loader->shutdown();
    CHECK(scenes->current() == nullptr);
    std::filesystem::remove(path);
}

TEST_CASE(failed_staged_load_preserves_current_scene)
{
    ayt::event::EventBus bus;
    auto* scenes = defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    auto loader = createRuntimeSceneLoader(*scenes, {}, &bus);
    CHECK(loader->initialize());
    auto* original = loader->currentScene();

    bool success = true;
    const auto connection = bus.subscribe<RuntimeSceneLoadFinishedEvent>(
        [&](const RuntimeSceneLoadFinishedEvent& event) {
            success = event.success;
        });

    CHECK(loader->requestLoad({1, "Z:/__ay_missing__/world.ayscene", "bad"}));
    loader->update(0.0f);
    CHECK(loader->currentScene() == original);
    CHECK(scenes->current() == original);
    CHECK(loader->getLoadStatus().state == RuntimeSceneLoadState::Failed);
    bus.pump();
    CHECK_FALSE(success);

    bus.unsubscribe(connection);
    loader->shutdown();
}

TEST_CASE(failed_activation_preparation_preserves_current_scene)
{
    ayt::event::EventBus bus;
    auto* scenes = defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    auto loader = createRuntimeSceneLoader(*scenes, {}, &bus);
    CHECK(loader->initialize());
    auto* original = loader->currentScene();
    const auto path = makeEmptySceneFile(
        "ay_runtime_scene_loader_prepare_fail.ayscene");

    RuntimeSceneLoadRequest request;
    request.requestId = 1;
    request.scenePath = path.string();
    request.sceneName = "rejected";
    request.prepareActivation = [](ayt::scene::Scene&, std::string& message) {
        message = "required content is unavailable";
        return false;
    };
    CHECK(loader->requestLoad(std::move(request)));
    loader->update(0.0f);
    CHECK(loader->currentScene() == original);
    CHECK(scenes->current() == original);
    CHECK(loader->getLoadStatus().message ==
          "required content is unavailable");

    loader->shutdown();
    std::filesystem::remove(path);
}

TEST_CASE(rejects_stale_request_ids_and_cancels_only_pending_request)
{
    ayt::event::EventBus bus;
    auto* scenes = defaultEngineHost().scenes();
    scenes->setCurrent(nullptr);
    auto loader = createRuntimeSceneLoader(*scenes, {}, &bus);
    CHECK(loader->initialize());

    CHECK_FALSE(loader->requestLoad({0, "map.ayscene", "map"}));
    CHECK(loader->requestLoad({10, "first.ayscene", "first"}));
    CHECK_FALSE(loader->requestLoad({10, "same.ayscene", "same"}));
    CHECK_FALSE(loader->cancelLoad(9));
    CHECK(loader->cancelLoad(10));
    CHECK(loader->getLoadStatus().state == RuntimeSceneLoadState::Cancelled);
    CHECK_FALSE(loader->requestLoad({9, "older.ayscene", "older"}));
    CHECK(loader->requestLoad({11, "newer.ayscene", "newer"}));

    loader->shutdown();
}

TEST_SUITE_END

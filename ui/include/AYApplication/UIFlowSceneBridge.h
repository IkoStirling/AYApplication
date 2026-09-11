#pragma once

#include <AYApplication/UIFlowRuntime.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::event
{
class EventBus;
}

namespace ayt::scene
{
class Scene;
}

namespace ayt::app
{

struct UIFlowWorldContextBinding
{
    std::string worldKey;
    std::string contextId;
};

using UIFlowWorldKeyResolver =
    std::function<std::string(const ayt::scene::Scene& scene)>;

struct UIFlowSceneBridgeConfig
{
    std::vector<UIFlowWorldContextBinding> worldContexts;
    UIFlowWorldKeyResolver worldKeyResolver;

    // Empty disables one lifecycle notification. Non-empty lifecycle Signals
    // are optional: they are emitted only when declared by the loaded Flow.
    std::string currentChangedSignal = "scene.currentChanged";
    std::string beginPlaySignal = "scene.beginPlay";
    std::string endPlaySignal = "scene.endPlay";

    bool enableSignalVolumes = true;
    bool processEditScenes = false;
};

// Generic event payload used by physics, script, task, or custom Scene systems
// that already perform their own overlap/interaction detection.
struct UIFlowSceneSignalEvent
{
    std::string sourceId;
    std::string sourceEntity;
    std::string participantEntity;
    std::string participantTag;
    UIFlowPayload payload;
};

struct UIFlowSceneBridgeStats
{
    std::uint64_t sceneChanges = 0;
    std::uint64_t volumeEnters = 0;
    std::uint64_t volumeExits = 0;
    std::uint64_t emittedSignals = 0;
    std::uint64_t rejectedSignals = 0;
};

// Main-thread Scene-to-Flow bridge. It owns no Scene, World, Entity, Widget,
// or UI layout. Scene lifetime enters/exits the runtime World scope; authored
// signal volumes and explicit game integrations publish typed Flow Signals.
class UIFlowSceneBridge
{
public:
    UIFlowSceneBridge(
        UIFlowRuntime& runtime,
        ayt::event::EventBus& eventBus,
        UIFlowSceneBridgeConfig config = {});
    ~UIFlowSceneBridge();

    UIFlowSceneBridge(const UIFlowSceneBridge&) = delete;
    UIFlowSceneBridge& operator=(const UIFlowSceneBridge&) = delete;

    bool start(
        ayt::scene::Scene* initialScene = nullptr,
        std::string* error = nullptr);
    void stop() noexcept;
    [[nodiscard]] bool isStarted() const noexcept;

    // Poll authored axis-aligned volumes for the active Scene. Calling with a
    // different Scene also repairs a missed lifecycle notification.
    bool update(
        ayt::scene::Scene* currentScene,
        std::string* error = nullptr);

    // Explicit entry point for physics/script/custom interaction systems.
    // Unlike optional lifecycle notifications, signalId must be declared.
    bool emitSceneSignal(
        std::string_view signalId,
        UIFlowSceneSignalEvent event = {},
        std::string* error = nullptr);

    [[nodiscard]] ayt::scene::Scene* currentScene() const noexcept;
    [[nodiscard]] std::string_view currentWorldKey() const noexcept;
    [[nodiscard]] const UIFlowSceneBridgeStats& stats() const noexcept;
    [[nodiscard]] std::string_view lastError() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ayt::event { class EventBus; }

namespace ayt::app
{

class GameFlowActionRegistry;
class GameFlowCoordinator;
class IEngineHost;
class IGameWorldRouter;
class IRuntimeSceneLoader;

inline constexpr std::string_view kGameFlowActionWorldReplace =
    "world.replace";

// Optional adapter that binds the headless GameFlow contract to the existing
// stable World router and RuntimeSceneLoader completion event. Every referenced
// service and registry must outlive this adapter.
class GameFlowWorldActionAdapter
{
public:
    GameFlowWorldActionAdapter(
        GameFlowActionRegistry& registry,
        GameFlowCoordinator& coordinator,
        IGameWorldRouter& router,
        IRuntimeSceneLoader& loader,
        ayt::event::EventBus& eventBus);
    ~GameFlowWorldActionAdapter();

    GameFlowWorldActionAdapter(const GameFlowWorldActionAdapter&) = delete;
    GameFlowWorldActionAdapter& operator=(
        const GameFlowWorldActionAdapter&) = delete;
    GameFlowWorldActionAdapter(GameFlowWorldActionAdapter&&) noexcept;
    GameFlowWorldActionAdapter& operator=(
        GameFlowWorldActionAdapter&&) noexcept;

    bool install(std::string* error = nullptr);
    void uninstall() noexcept;

    [[nodiscard]] bool installed() const noexcept;
    [[nodiscard]] std::uint64_t pendingSceneRequestId() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

// Convenience composition-root path. Resolves router, loader, and EventBus
// from one EngineHost, installs the adapter, and reports missing services.
[[nodiscard]] std::unique_ptr<GameFlowWorldActionAdapter>
createGameFlowWorldActionAdapter(
    IEngineHost& host,
    GameFlowActionRegistry& registry,
    GameFlowCoordinator& coordinator,
    std::string* error = nullptr);

} // namespace ayt::app
